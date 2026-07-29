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
  classifyRegion: classifyRegion,
  haversineDist: haversineDist,
  buildDestLines: buildDestLines,
  filterLiveData: filterLiveData,
};
