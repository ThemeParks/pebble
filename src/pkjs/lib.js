// lib.js — pure helper functions shared by index.js and tests/js/.
//
// Everything in here must stay side-effect free: no Pebble, no
// localStorage, no XMLHttpRequest, no require('message_keys'). That's
// what lets plain `node tests/js/test_lib.js` exercise this code without
// a phone runtime. It also has to run on the ancient PKJS interpreter,
// so: ES5-ish syntax only (var + function declarations).

// JS string length counts UTF-16 units; the wire format is UTF-8.
function utf8Len(s) {
  var n = 0;
  for (var i = 0; i < s.length; i++) {
    var c = s.charCodeAt(i);
    if (c < 0x80) n += 1;
    else if (c < 0x800) n += 2;
    else if (c >= 0xd800 && c < 0xdc00) { n += 4; i++; }  // surrogate pair
    else n += 3;
  }
  return n;
}

// Split `payload` into chunks of at most `limit` UTF-8 bytes, packing
// whole lines. The '\n' between lines packed into the same chunk is
// counted; the '\n' between two chunks is elided (the watch re-inserts
// it on reassembly, so chunks.join("\n") === payload). A single line
// longer than `limit` becomes its own oversized chunk rather than being
// split mid-line. Empty payload yields [""].
function splitIntoChunks(payload, limit) {
  var chunks = [];
  var lines = payload.split("\n");
  var cur = "";
  for (var i = 0; i < lines.length; i++) {
    if (cur && utf8Len(cur) + 1 + utf8Len(lines[i]) > limit) {
      chunks.push(cur);
      cur = lines[i];
    } else {
      cur = cur ? cur + "\n" + lines[i] : lines[i];
    }
  }
  chunks.push(cur);
  return chunks;
}

// Encode park data as pipe-delimited lines instead of JSON.
// This lets the watch parse without JSON.parse(), which creates dozens of
// objects that permanently consume XS chunk heap (no GC on Pebble).
//
// Format:
//   Line 1: schedule — "open|close" or empty
//   Line 2: lastUpdated timestamp
//   Lines:  "A|id|name|status|waitMins"   (attractions, waitMins may be empty)
//   Lines:  "S|id|name|status|t1,t2,..."  (shows, times comma-separated)
//   Lines:  "F|YYYY-MM-DD|open|close"     (upcoming days, from data.f —
//           optional; an absent/empty f emits nothing, so the pre-glance
//           payload shape still encodes byte-identically)
function encodeParkData(data) {
  var lines = [];
  // Schedule line
  if (data.sc) {
    lines.push(data.sc.o + "|" + data.sc.c);
  } else {
    lines.push("");
  }
  // Timestamp — SECONDS since epoch. data.u is milliseconds (Date.now())
  // everywhere on the phone side; the watch parses this line into a
  // 32-bit last_updated_secs, so sending ms would overflow the parse and
  // garble the header age suffix. Convert at the wire boundary.
  lines.push(String(Math.floor((data.u || Date.now()) / 1000)));
  // Attractions
  for (var i = 0; i < data.a.length; i++) {
    var a = data.a[i];
    lines.push("A|" + a[0] + "|" + a[1] + "|" + a[2] + "|" + (a[3] == null ? "" : a[3]));
  }
  // Shows
  for (var i = 0; i < data.h.length; i++) {
    var s = data.h[i];
    var times = s[3] ? s[3].join(",") : "";
    lines.push("S|" + s[0] + "|" + s[1] + "|" + s[2] + "|" + times);
  }
  // Upcoming days — includes today; the watch keys today's hours off
  // line 1 and consumes F| entries only for dates strictly after today
  // (dedupe convention documented in parkdata.c).
  if (data.f) {
    for (var i = 0; i < data.f.length; i++) {
      var fd = data.f[i];
      lines.push("F|" + fd.date + "|" + fd.o + "|" + fd.c);
    }
  }
  return lines.join("\n");
}

// Map full status to short code to save bytes
function shortStatus(s) {
  if (s === "OPERATING") return "O";
  if (s === "CLOSED") return "C";
  if (s === "DOWN") return "D";
  if (s === "REFURBISHMENT") return "R";
  return "?";
}

// Use last 8 hex chars of UUID as a compact ID (still unique within a park)
function shortId(id) {
  return id ? id.replace(/-/g, "").slice(-8) : "";
}

// Keep up to 36 chars so the watch renderer has slack to show more of
// the name on wide rows (middle of the gabbro circle, or the full-width
// stacked Shows row). The per-row GRect + text-flow handles ellipsis
// based on actual available width. 36 chars fits most Disney names in
// full ("The Twilight Zone Tower of Terror" is 34).
function shortName(name) {
  if (!name || name.length <= 36) return name;
  return name.slice(0, 35) + ".";
}

function formatTime(isoString) {
  // Extract HH:MM from the ISO string — the timezone offset in the string
  // already reflects the park's local wall clock. The wire format is the
  // zero-padded 24-hour "HH:MM" as-is; the WATCH formats it for display
  // per the user's 12/24-hour preference (format_clock_time in format.c),
  // so a Berlin user sees 21:30 while an Orlando user sees 9:30 PM.
  const match = isoString.match(/T(\d{2}):(\d{2})/);
  if (!match) return "";
  return match[1] + ":" + match[2];
}

// nowMs is optional (defaults to Date.now()) so tests can pin time.
function getTodaySchedule(schedule, todayStr, nowMs) {
  // Try an exact match first (fast path, correct for UTC-ish parks).
  for (var i = 0; i < schedule.length; i++) {
    var entry = schedule[i];
    if (entry.date === todayStr && entry.type === "OPERATING") {
      return {
        o: formatTime(entry.openingTime),
        c: formatTime(entry.closingTime),
      };
    }
  }
  // Fall back to "current or next OPERATING session" — needed for parks
  // east of UTC near midnight, where the park's local "today" has already
  // rolled over to the next UTC date.
  var now = nowMs === undefined ? Date.now() : nowMs;
  for (var j = 0; j < schedule.length; j++) {
    var e = schedule[j];
    if (e.type !== "OPERATING") continue;
    var closeMs = e.closingTime ? new Date(e.closingTime).getTime() : 0;
    if (closeMs > now) {
      return {
        o: formatTime(e.openingTime),
        c: formatTime(e.closingTime),
      };
    }
  }
  return null;
}

// Upcoming OPERATING days for the glance's multi-day slice queue: up to
// maxDays of {date:"YYYY-MM-DD", o:"HH:MM", c:"HH:MM"}, ordered by date,
// deduped by date (first OPERATING session per date wins — mirrors
// getTodaySchedule skipping TICKETED_EVENT entries). Today is included.
// "Upcoming" reuses getTodaySchedule's fallback thinking for parks east
// of UTC, where the park-local date can sit ahead of the UTC todayStr:
// rather than comparing against todayStr, an entry counts while its
// closingTime is still in the future — which also keeps a still-open
// overnight session dated "yesterday". Entries missing either time are
// dropped (the watch needs both to render a slice and set its expiry).
// nowMs is optional (defaults to Date.now()) so tests can pin time.
function getUpcomingSchedule(schedule, todayStr, maxDays, nowMs) {
  var now = nowMs === undefined ? Date.now() : nowMs;
  var sorted = schedule.slice().sort(function (a, b) {
    if (a.date < b.date) return -1;
    if (a.date > b.date) return 1;
    return 0;
  });
  var out = [];
  var seen = {};
  for (var i = 0; i < sorted.length && out.length < maxDays; i++) {
    var e = sorted[i];
    if (e.type !== "OPERATING") continue;
    if (seen[e.date]) continue;
    if (!e.openingTime || !e.closingTime) continue;
    if (new Date(e.closingTime).getTime() <= now) continue;
    seen[e.date] = true;
    out.push({
      date: e.date,
      o: formatTime(e.openingTime),
      c: formatTime(e.closingTime),
    });
  }
  return out;
}

// Build the destination-list payload lines sent to the watch:
//   X|nogps                 geolocation unavailable (watch shows a hint
//                           row where "Nearby" would be)
//   H|Section Name          section header ("Nearby", "Europe", ...)
//   D|id|name|parkCount|distKm   destination row (distKm 0 = unknown)
// `dests` come from fetchDestList(); userLoc is {lat, lon} or null.
function buildDestLines(dests, userLoc) {
  var lines = [];

  if (!userLoc) {
    lines.push("X|nogps");
  } else {
    // Nearby section: ten closest destinations with a known location.
    var withDist = [];
    for (var i = 0; i < dests.length; i++) {
      var d = dests[i];
      if (d.lat == null || d.lon == null) continue;
      withDist.push({
        dest: d,
        dist: haversineDist(userLoc.lat, userLoc.lon, d.lat, d.lon),
      });
    }
    withDist.sort(function (a, b) { return a.dist - b.dist; });
    withDist = withDist.slice(0, 10);
    if (withDist.length > 0) {
      lines.push("H|Nearby");
      for (var n = 0; n < withDist.length; n++) {
        var nd = withDist[n];
        lines.push("D|" + nd.dest.id + "|" + nd.dest.name + "|" +
                   nd.dest.parkCount + "|" + Math.round(nd.dist));
      }
    }
  }

  // Regions, alphabetical, each destination sorted by name.
  var regions = {};
  for (var j = 0; j < dests.length; j++) {
    var dd = dests[j];
    if (dd.lat == null) continue;
    var region = classifyRegion(dd.lat, dd.lon);
    if (!regions[region]) regions[region] = [];
    regions[region].push(dd);
  }
  var regionNames = Object.keys(regions).sort();
  for (var r = 0; r < regionNames.length; r++) {
    lines.push("H|" + regionNames[r]);
    var rDests = regions[regionNames[r]];
    rDests.sort(function (a, b) {
      if (a.name < b.name) return -1;
      if (a.name > b.name) return 1;
      return 0;
    });
    for (var rd = 0; rd < rDests.length; rd++) {
      lines.push("D|" + rDests[rd].id + "|" + rDests[rd].name + "|" +
                 rDests[rd].parkCount + "|0");
    }
  }

  return lines;
}

// Build the DEST_PARKS_DATA payload lines for a destination's parks:
//   M|fixed                  (only when isFixed) — tells the watch these
//                            parks came from the phone-side fixed
//                            destination setting, so it hides the
//                            destination picker and BACK exits
//   id|name|icon|color       one line per park
function buildParkLines(parks, isFixed) {
  var lines = [];
  if (isFixed) lines.push("M|fixed");
  for (var i = 0; i < parks.length; i++) {
    var pk = parks[i];
    lines.push(pk.id + "|" + pk.name + "|" + pk.icon + "|" + pk.color);
  }
  return lines;
}

// Whether a park-data request should be answered from the phone-side
// cache: it must exist, still be fresh, and the watch must not have set
// the force flag (manual "Refresh Data" — the user wants now-data).
function shouldServeCache(cached, nowMs, staleMs, force) {
  if (force) return false;
  if (!cached) return false;
  return nowMs - cached.timestamp < staleMs;
}

// Rough lat/lon bounding boxes. Not precise — just needs each destination
// to land in a sensible bucket.
function classifyRegion(lat, lon) {
  if (lat > 15 && lon < -50) return "North America";
  if (lat >= -55 && lat <= 15 && lon < -30) return "Latin America";
  if (lat > 35 && lon >= -15 && lon < 45) return "Europe";
  if (lat >= 15 && lat < 45 && lon >= 25 && lon < 65) return "Middle East & Africa";
  return "Asia Pacific";
}

function haversineDist(lat1, lon1, lat2, lon2) {
  var R = 6371; // km
  var toRad = function (d) { return d * Math.PI / 180; };
  var dLat = toRad(lat2 - lat1);
  var dLon = toRad(lon2 - lon1);
  var a = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
          Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) *
          Math.sin(dLon / 2) * Math.sin(dLon / 2);
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

// ── Phone-side settings ──────────────────────────────────────────────────
//
// Settings JSON (localStorage "tpw_settings" and the webview's close
// payload). Versioned from day one so later additions (units override,
// ...) migrate cleanly:
//   { v: 1, glance: true, destMode: "watch"|"fixed", destId: null|"<uuid>",
//     myParks: ["<parkUuid>", ...] }
// destMode/destId only mean anything in the uber build; per-destination
// builds carry them inert. myParks (cap MYPARKS_MAX) drives the packed
// multi-park glance line pushed to the watch.

var MYPARKS_MAX = 4;

function defaultSettings() {
  return { v: 1, glance: true, destMode: "watch", destId: null, myParks: [] };
}

// Tolerant parse of a stored/returned settings JSON string. Unknown
// fields are dropped, missing/mistyped fields get defaults, and any
// garbage (bad JSON, non-object, null input) yields pure defaults — the
// settings chain must never be able to wedge the app.
function parseSettings(json) {
  var s = defaultSettings();
  var p = null;
  try {
    p = typeof json === "string" ? JSON.parse(json) : json;
  } catch (e) {}
  if (!p || typeof p !== "object") return s;
  if (typeof p.v === "number" && isFinite(p.v)) s.v = p.v;
  if (typeof p.glance === "boolean") s.glance = p.glance;
  if (p.destMode === "watch" || p.destMode === "fixed") s.destMode = p.destMode;
  if (typeof p.destId === "string" && p.destId) s.destId = p.destId;
  // A fixed mode without a destination to fix on is meaningless — treat
  // it as "watch" so PKJS never tries to serve a null destination.
  if (s.destMode === "fixed" && !s.destId) s.destMode = "watch";
  // myParks: keep only non-empty strings, dedupe, truncate at the cap —
  // a hand-mangled localStorage entry degrades to fewer parks, never to
  // a wedged parse.
  if (Object.prototype.toString.call(p.myParks) === "[object Array]") {
    for (var i = 0; i < p.myParks.length && s.myParks.length < MYPARKS_MAX; i++) {
      var id = p.myParks[i];
      if (typeof id !== "string" || !id) continue;
      var dup = false;
      for (var j = 0; j < s.myParks.length; j++) {
        if (s.myParks[j] === id) { dup = true; break; }
      }
      if (!dup) s.myParks.push(id);
    }
  }
  return s;
}

// Compact wire string pushed to the watch under SETTINGS_DATA. Format:
// semicolon-separated key=value segments ("glance=1"); the watch parser
// (settings_parse.c) ignores unknown segments, so future fields append
// as ";key=value" without breaking older watch binaries. Sends only what
// the watch needs: the glance flag (destMode is applied phone-side) and,
// when lineText is non-null, the packed My Parks glance line as
// ";line=<text>|<expiryEpochSecs>". An empty lineText clears the watch's
// stored line. The text needs no escaping: buildGlanceLine guarantees it
// carries no ';', '=' or '|' (see settings_parse.c, which splits the
// value on its LAST '|' regardless).
function encodeSettingsPayload(settings, lineText, lineExpirySecs) {
  var out = "glance=" + (settings.glance ? "1" : "0");
  if (lineText != null) {
    out += ";line=" + lineText + "|" + (lineExpirySecs == null ? 0 : lineExpirySecs);
  }
  return out;
}

// ── My Parks glance line ─────────────────────────────────────────────────

// Short code for a park name, used in the packed My Parks glance line.
// Rules (unit-tested):
//   * Each word starting with an uppercase letter or a digit contributes
//     its first character; lowercase-led connectives ("of", "the") are
//     skipped. "Magic Kingdom" → "MK", "Islands of Adventure" → "IA",
//     "Epic Universe" → "EU", "Disney's Hollywood Studios" → "DHS".
//   * Codes cap at 3 characters ("Disney's Animal Kingdom Theme Park"
//     → "DAK").
//   * When fewer than 2 initials qualify (single-word names, whatever
//     their casing, and lowercase-only names), fall back to the first 3
//     alphanumeric characters of the name, uppercased: "EPCOT" → "EPC",
//     "Efteling" → "EFT".
//   * Output charset is [A-Z0-9] by construction — safe for the
//     ';'-separated settings wire format.
function parkCode(name) {
  if (!name) return "";
  // An "initial" is an uppercase letter or digit at the start of a word
  // OR an inner camelCase capital (previous char lowercase): device
  // testing found "Tokyo Disneyland"/"Tokyo DisneySea" both reducing to
  // "TD" — the inner S must count so DisneySea reads TDS.
  var s = String(name);
  var initials = "";
  var prev = " ";
  for (var i = 0; i < s.length && initials.length < 3; i++) {
    var c = s.charAt(i);
    var isUpperOrDigit = (c >= "A" && c <= "Z") || (c >= "0" && c <= "9");
    var wordStart = prev === " " || prev === "-";
    var innerCap = c >= "A" && c <= "Z" && prev >= "a" && prev <= "z";
    if (isUpperOrDigit && (wordStart || innerCap)) initials += c;
    prev = c === " " || c === "-" ? " " : c;
  }
  if (initials.length >= 2) return initials;
  var flat = s.replace(/[^A-Za-z0-9]/g, "");
  return flat.slice(0, 3).toUpperCase();
}

// Unique short codes for a set of park names. Codes that still collide
// (same initials, no camelCase help) get the first character at which
// the two flattened names differ appended, so e.g. two hypothetical
// "Magic Kingdom"/"Magic Kingdom West" render MKX/MKW-style instead of
// an ambiguous "MK 45m · MK 30m" line.
function assignParkCodes(names) {
  var codes = [];
  for (var i = 0; i < names.length; i++) codes.push(parkCode(names[i]));
  for (var a = 0; a < codes.length; a++) {
    for (var b = a + 1; b < codes.length; b++) {
      if (!codes[a] || codes[a] !== codes[b]) continue;
      var fa = String(names[a]).replace(/[^A-Za-z0-9]/g, "").toUpperCase();
      var fb = String(names[b]).replace(/[^A-Za-z0-9]/g, "").toUpperCase();
      /* Truly identical names (two "Legoland"s in different countries)
       * have no distinguishing letters — the shared code is honest. */
      if (fa === fb) continue;
      var j = 0;
      while (j < fa.length && j < fb.length && fa.charAt(j) === fb.charAt(j)) j++;
      codes[a] = (codes[a] + (j < fa.length ? fa.charAt(j) : "X")).slice(0, 4);
      codes[b] = (codes[b] + (j < fb.length ? fb.charAt(j) : "Y")).slice(0, 4);
    }
  }
  return codes;
}

// Shortest wait among operating attractions — the same "headline wait"
// shape the watch's glance composes (glance_headline in main.c), minus
// favourites, which don't apply phone-side. `attractions` are the
// compact [id, name, status, waitMins] arrays from filterLiveData.
// Returns null when nothing is operating with a known wait.
function shortestOperatingWait(attractions) {
  var best = null;
  for (var i = 0; i < attractions.length; i++) {
    var a = attractions[i];
    if (a[2] !== "O" || a[3] == null) continue;
    if (best === null || a[3] < best) best = a[3];
  }
  return best;
}

// Compose the packed My Parks glance line: "MK 45m · EP 30m · IA 60m".
// entries: [{name, wait, ageMs}] in display order (ts — absolute ms —
// is accepted in place of ageMs, aged against nowMs). Skipped entries:
// null/negative/non-numeric wait, unknown age, or age >= 30 min — the
// launcher must never show fossils. Budget: 60 chars (well under the
// ~150-byte slice cap even with 3-byte '·'s); trailing entries that
// would overflow are dropped. The result must reach the wire unescaped,
// so it is guaranteed free of ';' and '=' (settings segment/kv
// separators) and '|' (the line/expiry separator): park codes are
// [A-Z0-9], everything else is composed here, and a final strip
// enforces it against future edits. Returns "" when nothing is usable.
var GLANCE_LINE_MAX_CHARS = 60;
var GLANCE_LINE_STALE_MS = 30 * 60 * 1000;
function buildGlanceLine(entries, nowMs) {
  var now = nowMs === undefined ? Date.now() : nowMs;
  // Codes assigned over the whole set, collision-disambiguated, so two
  // similarly named parks never render as the same code.
  var names = [];
  for (var n = 0; n < entries.length; n++) {
    names.push(entries[n] ? entries[n].name : "");
  }
  var codes = assignParkCodes(names);
  var out = "";
  for (var i = 0; i < entries.length; i++) {
    var e = entries[i];
    if (!e) continue;
    if (typeof e.wait !== "number" || !isFinite(e.wait) || e.wait < 0) continue;
    var age = e.ageMs != null ? e.ageMs : (e.ts != null ? now - e.ts : null);
    if (age == null || age >= GLANCE_LINE_STALE_MS) continue;
    var code = codes[i].replace(/[;=|]/g, "");
    if (!code) continue;
    var part = code + " " + Math.round(e.wait) + "m";
    var candidate = out ? out + " · " + part : part;
    if (candidate.length > GLANCE_LINE_MAX_CHARS) break;
    out = candidate;
  }
  return out.replace(/[;=|]/g, "");
}

// Minimal HTML escaping for text nodes and double-quoted attributes.
function escapeHtml(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

// Build the full self-contained config-page HTML (inline CSS/JS, no
// external resources — the phone webview may be offline). `dests` is the
// fetchDestList() result array ([] when unavailable); isUber gates the
// Destination section. `parks` ([{id, name}]) feeds the My Parks section
// in per-destination builds; the uber build ignores it and lists every
// destination's parks (dests[].parks) grouped by destination instead.
// Page JS stays ES5 — some phone webviews are as old as the PKJS
// interpreter.
//
// Save navigates to pebblejs://close#<encodeURIComponent(settings JSON)>;
// Cancel navigates to pebblejs://close with no payload — index.js treats
// an absent/empty e.response as "no change".
function buildConfigPage(settings, dests, isUber, parks) {
  var h = [];
  h.push("<!DOCTYPE html><html><head>");
  h.push('<meta charset="utf-8">');
  h.push('<meta name="viewport" content="width=device-width, initial-scale=1">');
  h.push("<title>Theme Parks Settings</title>");
  h.push("<style>");
  h.push("body{font-family:sans-serif;margin:0;padding:16px;background:#f4f4f4;color:#222;}");
  h.push("h1{font-size:1.3em;margin:0 0 12px;}");
  h.push(".card{background:#fff;border-radius:6px;padding:12px;margin-bottom:12px;box-shadow:0 1px 2px rgba(0,0,0,0.15);}");
  h.push(".card h2{font-size:1em;margin:0 0 8px;}");
  h.push(".card h3{font-size:0.85em;margin:10px 0 2px;color:#555;}");
  h.push("label{display:block;margin:6px 0;}");
  h.push("select{width:100%;margin-top:6px;}");
  h.push(".hint{font-size:0.85em;color:#666;margin:6px 0 0;}");
  h.push(".buttons{display:flex;gap:8px;}");
  h.push("button{flex:1;padding:10px;font-size:1em;border:0;border-radius:6px;}");
  h.push("#save{background:#f9a825;color:#222;font-weight:bold;}");
  h.push("#cancel{background:#ddd;color:#222;}");
  h.push("</style></head><body>");
  h.push("<h1>Theme Parks Settings</h1>");

  // AppGlance toggle
  h.push('<div class="card"><h2>Launcher</h2>');
  h.push('<label><input type="checkbox" id="glance"'
         + (settings.glance ? " checked" : "") + "> Show park info in the launcher (AppGlance)</label>");
  h.push("</div>");

  if (isUber) {
    var haveDests = dests && dests.length > 0;
    var fixed = settings.destMode === "fixed" && haveDests;
    h.push('<div class="card"><h2>Destination</h2>');
    h.push('<label><input type="radio" name="destmode" value="watch"'
           + (fixed ? "" : " checked") + "> Choose on watch</label>");
    h.push('<label><input type="radio" name="destmode" value="fixed"'
           + (fixed ? " checked" : "") + (haveDests ? "" : " disabled")
           + "> Fixed destination</label>");
    if (haveDests) {
      // Group by region (same buckets as the watch's destination picker);
      // location-less destinations go under "Other" rather than being
      // dropped — a user must be able to fix on any destination the watch
      // could reach. Alphabetical groups; "Other" happens to sort last.
      var groups = {};
      for (var i = 0; i < dests.length; i++) {
        var d = dests[i];
        var region = (d.lat == null || d.lon == null)
          ? "Other" : classifyRegion(d.lat, d.lon);
        if (!groups[region]) groups[region] = [];
        groups[region].push(d);
      }
      var names = Object.keys(groups).sort();
      h.push('<select id="destid"' + (fixed ? "" : " disabled") + ">");
      for (var g = 0; g < names.length; g++) {
        h.push('<optgroup label="' + escapeHtml(names[g]) + '">');
        var gd = groups[names[g]];
        gd.sort(function (a, b) {
          if (a.name < b.name) return -1;
          if (a.name > b.name) return 1;
          return 0;
        });
        for (var j = 0; j < gd.length; j++) {
          h.push('<option value="' + escapeHtml(gd[j].id) + '"'
                 + (settings.destId === gd[j].id ? " selected" : "") + ">"
                 + escapeHtml(gd[j].name) + "</option>");
        }
        h.push("</optgroup>");
      }
      h.push("</select>");
    } else {
      h.push('<p class="hint">Destination list unavailable — check the phone\'s '
             + "internet connection and reopen this page.</p>");
    }
    h.push("</div>");
  }

  // My Parks multi-select (cap MYPARKS_MAX): the parks whose live waits
  // get packed into the launcher glance line. Uber: every destination's
  // parks, grouped by destination name (alphabetical, matching the
  // dropdown's data source); per-destination builds: the build's own
  // parks via `parks`. Prefilled from settings.myParks; the cap is
  // enforced by page JS (unchecked boxes disable at the cap) AND
  // re-clamped in the save handler.
  var myParks = settings.myParks || [];
  function checkedAttr(id) {
    for (var m = 0; m < myParks.length; m++) {
      if (myParks[m] === id) return " checked";
    }
    return "";
  }
  function parkBox(pk) {
    h.push('<label><input type="checkbox" name="mypark" value="'
           + escapeHtml(pk.id) + '"' + checkedAttr(pk.id) + "> "
           + escapeHtml(pk.name) + "</label>");
  }
  h.push('<div class="card"><h2>My Parks</h2>');
  h.push('<p class="hint">Pick up to 4 parks to show live waits for in the launcher.</p>');
  var anyParks = false;
  if (isUber) {
    if (dests && dests.length > 0) {
      var sortedDests = dests.slice().sort(function (a, b) {
        if (a.name < b.name) return -1;
        if (a.name > b.name) return 1;
        return 0;
      });
      for (var sd = 0; sd < sortedDests.length; sd++) {
        var dp = sortedDests[sd].parks || [];
        if (dp.length === 0) continue;
        anyParks = true;
        h.push("<h3>" + escapeHtml(sortedDests[sd].name) + "</h3>");
        for (var pp = 0; pp < dp.length; pp++) parkBox(dp[pp]);
      }
    }
  } else if (parks && parks.length > 0) {
    anyParks = true;
    for (var op = 0; op < parks.length; op++) parkBox(parks[op]);
  }
  if (!anyParks) {
    // Same degradation as the dropdown: the page still opens, this
    // section just can't offer anything. (Wording avoids the exact
    // dropdown hint so tests can tell the two apart.)
    h.push('<p class="hint">Park list unavailable — check the phone\'s '
           + "internet connection and reopen this page.</p>");
  }
  h.push("</div>");

  h.push('<div class="buttons">');
  h.push('<button id="cancel" type="button">Cancel</button>');
  h.push('<button id="save" type="button">Save</button>');
  h.push("</div>");

  // Page JS — ES5 only. Reads the controls back into the settings JSON
  // and hands it to PKJS via the pebblejs://close hash.
  h.push("<script>");
  h.push("function radioVal(name){var r=document.getElementsByName(name);"
         + "for(var i=0;i<r.length;i++){if(r[i].checked)return r[i].value;}return null;}");
  h.push("function setDestEnabled(){var s=document.getElementById('destid');"
         + "if(s){s.disabled=radioVal('destmode')!=='fixed';}}");
  h.push("var radios=document.getElementsByName('destmode');"
         + "for(var i=0;i<radios.length;i++){radios[i].onchange=setDestEnabled;}");
  // My Parks cap: once MYPARKS_MAX boxes are checked, disable the
  // unchecked ones; un-checking re-enables. Runs on load too so a
  // prefilled-at-cap page starts locked.
  h.push("function updateMyParksCap(){var b=document.getElementsByName('mypark');"
         + "var n=0;var i;for(i=0;i<b.length;i++){if(b[i].checked)n++;}"
         + "for(i=0;i<b.length;i++){if(!b[i].checked)b[i].disabled=(n>=" + MYPARKS_MAX + ");}}");
  h.push("var mpb=document.getElementsByName('mypark');"
         + "for(var mi=0;mi<mpb.length;mi++){mpb[mi].onchange=updateMyParksCap;}"
         + "updateMyParksCap();");
  h.push("document.getElementById('cancel').onclick=function(){"
         + "document.location='pebblejs://close';};");
  h.push("document.getElementById('save').onclick=function(){"
         + "var s={v:1};"
         + "s.glance=document.getElementById('glance').checked;"
         + "var mode=radioVal('destmode');"
         + "var sel=document.getElementById('destid');"
         + "s.destMode=(mode==='fixed'&&sel&&sel.value)?'fixed':'watch';"
         + "s.destId=(s.destMode==='fixed')?sel.value:null;"
         + "s.myParks=[];"
         + "var mp=document.getElementsByName('mypark');"
         + "for(var pi=0;pi<mp.length;pi++){"
         + "if(mp[pi].checked&&s.myParks.length<" + MYPARKS_MAX + ")s.myParks.push(mp[pi].value);}"
         + "document.location='pebblejs://close#'+encodeURIComponent(JSON.stringify(s));};");
  h.push("</" + "script>");
  h.push("</body></html>");
  return h.join("");
}

// nowMs is optional (defaults to Date.now()) so tests can pin time.
function filterLiveData(liveData, nowMs) {
  const attractions = [];
  const shows = [];
  const now = nowMs === undefined ? Date.now() : nowMs;

  for (let i = 0; i < liveData.length; i++) {
    const entity = liveData[i];
    if (entity.entityType === "ATTRACTION") {
      const waitTime = entity.queue && entity.queue.STANDBY ? entity.queue.STANDBY.waitTime : null;
      // Compact array format: [id, name, status, waitMins]
      attractions.push([
        shortId(entity.id),
        shortName(entity.name),
        shortStatus(entity.status),
        waitTime == null ? null : waitTime,
      ]);
    } else if (entity.entityType === "SHOW") {
      const times = [];
      if (entity.showtimes) {
        // Keep only next 4 upcoming, pre-formatted to short time strings
        for (let j = 0; j < entity.showtimes.length && times.length < 4; j++) {
          const t = entity.showtimes[j].startTime;
          if (new Date(t).getTime() > now) {
            times.push(formatTime(t));
          }
        }
      }
      // Compact array format: [id, name, status, times]
      shows.push([
        shortId(entity.id),
        shortName(entity.name),
        shortStatus(entity.status),
        times,
      ]);
    }
  }

  return { a: attractions, h: shows };
}

module.exports = {
  utf8Len: utf8Len,
  splitIntoChunks: splitIntoChunks,
  encodeParkData: encodeParkData,
  shortStatus: shortStatus,
  shortId: shortId,
  shortName: shortName,
  formatTime: formatTime,
  shouldServeCache: shouldServeCache,
  getTodaySchedule: getTodaySchedule,
  getUpcomingSchedule: getUpcomingSchedule,
  classifyRegion: classifyRegion,
  haversineDist: haversineDist,
  buildDestLines: buildDestLines,
  buildParkLines: buildParkLines,
  filterLiveData: filterLiveData,
  defaultSettings: defaultSettings,
  parseSettings: parseSettings,
  encodeSettingsPayload: encodeSettingsPayload,
  escapeHtml: escapeHtml,
  buildConfigPage: buildConfigPage,
  MYPARKS_MAX: MYPARKS_MAX,
  parkCode: parkCode,
  assignParkCodes: assignParkCodes,
  shortestOperatingWait: shortestOperatingWait,
  buildGlanceLine: buildGlanceLine,
};
