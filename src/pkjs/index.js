// PKJS — runs on the phone. Does all the API fetching, filtering, and
// transformation. Sends compact payloads to the watch via AppMessage.
//
// Message protocol: keys are declared in `messageKeys` in package.json and
// numbered by the SDK from 10000 in declaration order. The build-generated
// name→number map comes from require('message_keys').
//
// IMPORTANT: incoming e.payload is keyed by the key NAME on real phone
// apps (the documented PKJS interface). pypkjs (the emulator runtime)
// additionally includes the numeric keys, which is why numeric-only
// lookups appear to work in the emulator but silently receive nothing on
// a physical watch. Always read via msgVal() and send by name.
var MSG_KEYS = require('message_keys');

// Pure helpers (encoding, chunk-splitting, time/region math) live in
// lib.js so the JS unit tests can exercise them without a phone runtime.
var lib = require('./lib');
var utf8Len = lib.utf8Len;
var splitIntoChunks = lib.splitIntoChunks;
var encodeParkData = lib.encodeParkData;
var shouldServeCache = lib.shouldServeCache;
var getTodaySchedule = lib.getTodaySchedule;
var getUpcomingSchedule = lib.getUpcomingSchedule;
var classifyRegion = lib.classifyRegion;
var haversineDist = lib.haversineDist;
var buildDestLines = lib.buildDestLines;
var buildParkLines = lib.buildParkLines;
var filterLiveData = lib.filterLiveData;
var parseSettings = lib.parseSettings;
var encodeSettingsPayload = lib.encodeSettingsPayload;
var buildConfigPage = lib.buildConfigPage;
var shortestOperatingWait = lib.shortestOperatingWait;
var buildGlanceLine = lib.buildGlanceLine;
var parseFavsSync = lib.parseFavsSync;
var buildFavsStore = lib.buildFavsStore;

function msgVal(payload, name) {
  if (payload[name] !== undefined) return payload[name];
  return payload[MSG_KEYS[name]];
}

const API_BASE = "https://api.themeparks.wiki/v1";
// Just under the watch's 5-minute auto-refresh cadence, so each refresh
// fetches live data instead of replaying this cache. The cache still
// serves quick park re-entry and the stale-fallback on fetch errors.
const STALE_THRESHOLD_MS = 4 * 60 * 1000;
const CACHE_KEY_PREFIX = "tpw_cache_";

// Destination ID — used to fetch all park schedules in one request.
// Patched per-destination by build_dest.py alongside ALL_PARK_IDS.
// `var` (not const) so uber-app mode can reassign at runtime once the
// user picks a destination.
var DESTINATION_ID = "e957da41-3552-4cf6-b636-5babc5cbc4e5";

// All parks PKJS can fetch hours for — mirrors the watch-side g_parks
// table but PKJS doesn't *need* to know names, just IDs. When the watch
// asks for all hours we iterate and reply with one line per park.
var ALL_PARK_IDS = [
  "75ea578a-adc8-4116-a54d-dccb60765ef9",   // Magic Kingdom
  "47f90d2c-e191-4239-a466-5892ef59a88b",   // EPCOT
  "288747d1-8b4f-4a64-867e-ea7c9b27bad8",   // Hollywood Studios
  "1c84a229-8862-4648-9c71-378ddd2c7693",   // Animal Kingdom
];

// Uber-build flag, captured before any runtime reassignment of
// DESTINATION_ID (handleDestSelect overwrites it once the user picks a
// destination, so testing DESTINATION_ID directly later would lie).
var IS_UBER = DESTINATION_ID === null;

// In-memory cache. Persisted entries are loaded lazily on first request.
const cache = {};

// ── Phone-side settings (config page) ────────────────────────────────────

var SETTINGS_KEY = "tpw_settings";

function loadSettings() {
  var raw = null;
  try { raw = localStorage.getItem(SETTINGS_KEY); } catch (e) {}
  return parseSettings(raw);
}

function saveSettings(settings) {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
  } catch (e) {
    console.log("PKJS: settings save failed - " + e.message);
  }
}

// Send a settings wire payload (see lib.encodeSettingsPayload) to the
// watch. It's a tiny single message — plain send with a couple of
// retries, no chunking needed.
function sendSettingsPayload(payload) {
  function attempt(retriesLeft) {
    var msg = {};
    msg.SETTINGS_DATA = payload;
    Pebble.sendAppMessage(
      msg,
      function () { console.log("PKJS: settings pushed (" + payload + ")"); },
      function () {
        if (retriesLeft > 0) {
          setTimeout(function () { attempt(retriesLeft - 1); }, 500);
        } else {
          console.log("PKJS: settings push failed permanently");
        }
      }
    );
  }
  attempt(2);
}

// Push the settings payload, with the packed My Parks glance line
// appended when the user has selected parks (see buildMyParksLine). No
// selection pushes an explicit empty line segment, which clears any
// line the watch still has persisted from an earlier selection.
function pushSettingsToWatch(settings) {
  var myParks = settings.myParks || [];
  if (myParks.length === 0) {
    sendSettingsPayload(lib.encodeSettingsPayload(settings, "", 0));
    return;
  }
  buildMyParksLine(myParks).then(function (line) {
    // Expiry: the glance must never outlive the data's 30-minute
    // freshness window. Seconds — the watch compares against time(NULL).
    var expiry = line ? Math.floor(Date.now() / 1000) + GLANCE_LINE_TTL_SECS : 0;
    sendSettingsPayload(lib.encodeSettingsPayload(settings, line, expiry));
  });
}

function openConfigPage(settings, dests, parks) {
  var html = buildConfigPage(settings, dests, IS_UBER, parks);
  Pebble.openURL("data:text/html;charset=utf-8," + encodeURIComponent(html));
}

// The build's own parks ({id, name}) for the per-destination config
// page's My Parks section: the API destination's park list filtered to
// the parks this build actually ships (the destination may carry water
// parks etc. that the watch app doesn't).
function ownParksFrom(dests) {
  var out = [];
  for (var i = 0; i < dests.length; i++) {
    if (dests[i].id !== DESTINATION_ID) continue;
    var parks = dests[i].parks || [];
    for (var p = 0; p < parks.length; p++) {
      for (var k = 0; k < ALL_PARK_IDS.length; k++) {
        if (parks[p].id === ALL_PARK_IDS[k]) {
          out.push({ id: parks[p].id, name: parks[p].name });
          break;
        }
      }
    }
    break;
  }
  return out;
}

Pebble.addEventListener("showConfiguration", function () {
  var settings = loadSettings();
  // Both builds want the dest list (cached 24h) now: the uber build for
  // the destination dropdown + the My Parks grouping, per-destination
  // builds for their own parks' display names in the My Parks section.
  // On failure the page still opens with those sections degraded to
  // hints instead of never opening.
  fetchDestList().then(function (dests) {
    if (IS_UBER) openConfigPage(settings, dests, []);
    else openConfigPage(settings, [], ownParksFrom(dests));
  }).catch(function (err) {
    console.log("PKJS: config dest list failed: " + err.message);
    openConfigPage(settings, [], []);
  });
});

Pebble.addEventListener("webviewclosed", function (e) {
  if (!e || !e.response) {
    console.log("PKJS: config page dismissed, no changes");
    return;
  }
  var raw = e.response;
  try { raw = decodeURIComponent(raw); } catch (err) {}
  // Snapshot the pre-save pins flag: the true→false transition is the
  // cleanup trigger (delete everything we ever pushed, best effort).
  var pinsWereOn = loadSettings().pins;
  // parseSettings is garbage-tolerant, so a mangled payload degrades to
  // defaults rather than throwing here.
  var settings = parseSettings(raw);
  saveSettings(settings);
  console.log("PKJS: settings saved: " + JSON.stringify(settings));
  pushSettingsToWatch(settings);
  if (pinsWereOn && !settings.pins) {
    disableTimelinePins();
  } else if (settings.pins) {
    // Freshly enabled (or scope changed): push what we can from the
    // data already fetched this session. Parks with no fetched
    // schedule yet pick up their pins on the next serve.
    syncTimelinePins(lastServedParkId);
  }
  if (IS_UBER && settings.destMode === "fixed" && settings.destId) {
    // Apply immediately: a running watch app gets the fixed
    // destination's parks pushed right away (same path as a watch-side
    // DEST_SELECT), instead of waiting for its next dest request.
    console.log("PKJS: applying fixed destination " + settings.destId);
    handleDestSelect(settings.destId, true);
  }
  // Switching back to "choose on watch" needs no push: the watch's
  // fixed flag clears on the next unmarked parks payload, i.e. the
  // next destination pick or relaunch resume.
});

const DEST_CACHE_KEY = "tpw_dest_cache";
const DEST_CACHE_TTL_MS = 24 * 60 * 60 * 1000;

// Icon + colour overrides for known parks. Parks not listed here get a
// cycling palette colour and a "circle" icon (falls back in park_picker.c).
const PARK_STYLE = {
  // Walt Disney World
  "75ea578a-adc8-4116-a54d-dccb60765ef9": { icon: "castle", color: "pink" },
  "47f90d2c-e191-4239-a466-5892ef59a88b": { icon: "sphere", color: "cyan" },
  "288747d1-8b4f-4a64-867e-ea7c9b27bad8": { icon: "star",   color: "red" },
  "1c84a229-8862-4648-9c71-378ddd2c7693": { icon: "tree",   color: "green" },
};

const DEFAULT_PALETTE = ["pink", "cyan", "red", "green", "orange", "blue", "purple", "yellow"];

// AppMessage payloads must fit the watch's inbox (8KB, see api.c). The
// full destination list is ~11KB and growing, so big payloads are split
// into chunks at newline boundaries and reassembled watch-side. The
// emulator's pypkjs ignores the negotiated inbox size, which is why
// oversized single messages "work" there and silently fail on hardware.
// Budget leaves room for dict/tuple overhead + the two chunk-key tuples.
var CHUNK_BYTE_LIMIT = 6000;

// Send `payload` under key `keyName`, split into <=CHUNK_BYTE_LIMIT chunks
// at newline boundaries (see lib.splitIntoChunks). Each message carries
// CHUNK_INDEX and CHUNK_MORE; the watch reassembles and dispatches when
// CHUNK_MORE is 0. Chunks are sent serially, gated on the previous
// chunk's ack, with a few retries.
function sendChunked(keyName, payload, onDone, onFail) {
  var chunks = splitIntoChunks(payload, CHUNK_BYTE_LIMIT);

  function sendOne(idx, retriesLeft) {
    var msg = {};
    msg[keyName] = chunks[idx];
    msg.CHUNK_INDEX = idx;
    msg.CHUNK_MORE = idx < chunks.length - 1 ? 1 : 0;
    Pebble.sendAppMessage(
      msg,
      function () {
        if (idx + 1 < chunks.length) sendOne(idx + 1, 3);
        else if (onDone) onDone();
      },
      function (e) {
        if (retriesLeft > 0) {
          console.log("PKJS: chunk " + idx + " of " + keyName + " failed, retrying");
          setTimeout(function () { sendOne(idx, retriesLeft - 1); }, 500);
        } else {
          console.log("PKJS: chunk " + idx + " of " + keyName + " failed permanently");
          if (onFail) onFail(e);
        }
      }
    );
  }
  console.log("PKJS: sending " + keyName + " in " + chunks.length + " chunk(s)");
  sendOne(0, 3);
}

function getParkStyle(parkId, indexInDest) {
  var style = PARK_STYLE[parkId];
  if (style) return style;
  return {
    icon: "circle",
    color: DEFAULT_PALETTE[indexInDest % DEFAULT_PALETTE.length],
  };
}

function getLocation() {
  // Test hook: the integration harness seeds this localStorage key to
  // pin GPS (pypkjs geolocates by the host's datacenter IP, which makes
  // the "Nearby" list nondeterministic in tests). Real phones never
  // have the key.
  try {
    var fake = localStorage.getItem("tpw_test_location");
    if (fake) {
      var parts = String(fake).split(",");
      var la = parseFloat(parts[0]);
      var lo = parseFloat(parts[1]);
      if (!isNaN(la) && !isNaN(lo)) {
        return Promise.resolve({ lat: la, lon: lo });
      }
    }
  } catch (e) {}
  return new Promise(function (resolve) {
    if (typeof navigator === "undefined" || !navigator.geolocation) {
      resolve(null);
      return;
    }
    // Belt-and-braces timer: some phone runtimes never invoke either
    // callback (e.g. location permission denied to the Pebble app at the
    // OS level), and the `timeout` option isn't honoured everywhere.
    // Without this the destination list would never be sent.
    var done = false;
    var fallback = null;
    function finish(loc) {
      if (done) return;
      done = true;
      if (fallback !== null) clearTimeout(fallback);
      resolve(loc);
    }
    fallback = setTimeout(function () { finish(null); }, 12000);
    // Some phone runtimes throw synchronously from getCurrentPosition
    // (e.g. location permission denied at the OS level). An uncaught
    // throw here would reject the promise and kill the whole dest-list
    // build — degrade to "no location" instead.
    try {
      navigator.geolocation.getCurrentPosition(
        function (pos) {
          finish({ lat: pos.coords.latitude, lon: pos.coords.longitude });
        },
        function () { finish(null); },
        { timeout: 10000, maximumAge: 60000 }
      );
    } catch (e) {
      console.log("PKJS: geolocation threw: " + e.message);
      finish(null);
    }
  });
}

// Fetch destination list + park locations from API, cache for 24h.
// Returns a Promise resolving to an array of:
//   { id, name, parkCount, lat, lon,
//     parks: [{ id, name, icon, color }, ...] }
function fetchDestList() {
  var cached = null;
  try {
    var raw = localStorage.getItem(DEST_CACHE_KEY);
    if (raw) {
      cached = JSON.parse(raw);
      if (cached && (Date.now() - cached.timestamp < DEST_CACHE_TTL_MS)) {
        return Promise.resolve(cached.data);
      }
    }
  } catch (e) {}

  return Promise.all([
    xhrGet(API_BASE + "/destinations"),
    xhrGet(API_BASE + "/map"),
  ]).then(function (results) {
    var destResp = results[0];
    var mapResp = results[1];

    // Lat/lon lookup keyed by park id
    var parkLocs = {};
    var mapParks = mapResp.parks || [];
    for (var i = 0; i < mapParks.length; i++) {
      var mp = mapParks[i];
      if (mp.location) parkLocs[mp.id] = mp.location;
    }

    var dests = destResp.destinations || destResp;
    var result = [];
    for (var d = 0; d < dests.length; d++) {
      var dest = dests[d];
      var parks = dest.parks || [];
      var loc = null;
      for (var p = 0; p < parks.length; p++) {
        if (parkLocs[parks[p].id]) {
          loc = parkLocs[parks[p].id];
          break;
        }
      }
      var parkInfos = [];
      for (var pi = 0; pi < parks.length; pi++) {
        var style = getParkStyle(parks[pi].id, pi);
        parkInfos.push({
          id: parks[pi].id,
          name: parks[pi].name,
          icon: style.icon,
          color: style.color,
        });
      }
      result.push({
        id: dest.id,
        name: dest.name,
        parkCount: parks.length,
        lat: loc ? loc.latitude : null,
        lon: loc ? loc.longitude : null,
        parks: parkInfos,
      });
    }

    try {
      localStorage.setItem(DEST_CACHE_KEY, JSON.stringify({
        timestamp: Date.now(),
        data: result,
      }));
    } catch (e) {}

    return result;
  });
}

// Build + send the destination list payload to the watch. Pipe-delimited:
//   H|Section Name           header row (e.g. "Nearby", "Europe")
//   D|id|name|parkCount|distKm   destination row (distKm=0 if unknown)
function buildAndSendDestList() {
  Promise.all([fetchDestList(), getLocation()]).then(function (results) {
    var dests = results[0];
    var userLoc = results[1];

    // Line building lives in lib.js (buildDestLines) so it's unit
    // tested — including the X|nogps marker the watch renders as a
    // "No location" hint where the Nearby section would be.
    var payload = buildDestLines(dests, userLoc).join("\n");
    console.log("PKJS: sending dest list (" + utf8Len(payload) + " bytes, " +
                dests.length + " destinations)");
    sendChunked("DEST_LIST_DATA", payload,
      function () { console.log("PKJS: dest list sent ok"); },
      function () { console.log("PKJS: dest list send failed"); }
    );
  }).catch(function (err) {
    console.log("PKJS: dest list error: " + err.message);
  });
}

// Watch picked a destination (or the fixed-destination setting did) —
// update DESTINATION_ID/ALL_PARK_IDS and send the park list back to the
// watch. `isFixed` prepends the M|fixed marker so the watch switches to
// per-destination-style navigation (no dest picker, BACK exits).
function handleDestSelect(destId, isFixed) {
  fetchDestList().then(function (dests) {
    var dest = null;
    for (var i = 0; i < dests.length; i++) {
      if (dests[i].id === destId) { dest = dests[i]; break; }
    }
    if (!dest) {
      console.log("PKJS: unknown destination " + destId);
      return;
    }

    DESTINATION_ID = destId;
    ALL_PARK_IDS.length = 0;
    for (var p = 0; p < dest.parks.length; p++) {
      ALL_PARK_IDS.push(dest.parks[p].id);
    }

    var payload = buildParkLines(dest.parks, !!isFixed).join("\n");
    console.log("PKJS: sending parks for " + dest.name + " (" + dest.parks.length + " parks)");
    sendChunked("DEST_PARKS_DATA", payload,
      function () {
        console.log("PKJS: dest parks sent ok");
        fetchAllHours();
      },
      function () { console.log("PKJS: dest parks send failed"); }
    );
  }).catch(function (err) {
    console.log("PKJS: dest select error: " + err.message);
  });
}

function loadCache(parkId) {
  if (cache[parkId]) return cache[parkId];
  try {
    const raw = localStorage.getItem(CACHE_KEY_PREFIX + parkId);
    if (raw) {
      const parsed = JSON.parse(raw);
      cache[parkId] = parsed;
      return parsed;
    }
  } catch (err) {
    console.log("PKJS: cache load failed - " + err.message);
  }
  return null;
}

function saveCache(parkId, data) {
  const entry = { data: data, timestamp: Date.now() };
  cache[parkId] = entry;
  try {
    localStorage.setItem(CACHE_KEY_PREFIX + parkId, JSON.stringify(entry));
  } catch (err) {
    console.log("PKJS: cache save failed - " + err.message);
  }
  // Every fresh fetch also refreshes the per-park wait snapshot the
  // My Parks glance line is composed from.
  recordWaitSnapshot(parkId, data, entry.timestamp);
}

// ── My Parks glance line ─────────────────────────────────────────────────
//
// The launcher glance can show a packed multi-park wait line ("MK 45m ·
// EP 30m") for the parks picked on the config page. The watch only ever
// holds one park's live data, so PKJS composes the line and appends it
// to the settings payload (";line=<text>|<expiry>") after every park
// serve and on every settings push. tpw_waits keeps the latest
// {name, wait, ts} snapshot per park id, refreshed on every cache save.

var WAITS_KEY = "tpw_waits";
var GLANCE_LINE_TTL_SECS = 30 * 60;

function loadWaits() {
  try {
    var raw = localStorage.getItem(WAITS_KEY);
    if (raw) {
      var p = JSON.parse(raw);
      if (p && typeof p === "object") return p;
    }
  } catch (e) {}
  return {};
}

// Park display name from the cached dest list — synchronous (no fetch);
// null while the cache hasn't been populated yet. Callers that need a
// name go through fetchDestList() first, which fills the cache.
function parkNameFor(parkId) {
  try {
    var raw = localStorage.getItem(DEST_CACHE_KEY);
    if (!raw) return null;
    var cached = JSON.parse(raw);
    var dests = cached && cached.data;
    if (!dests) return null;
    for (var d = 0; d < dests.length; d++) {
      var parks = dests[d].parks || [];
      for (var p = 0; p < parks.length; p++) {
        if (parks[p].id === parkId) return parks[p].name;
      }
    }
  } catch (e) {}
  return null;
}

// Record {name, shortestOperatingWait, ts} for a park. ts defaults to
// now; the warm-from-cache path passes the cache entry's timestamp so a
// stale cache can't masquerade as a fresh wait.
function recordWaitSnapshot(parkId, data, tsOpt) {
  var w = loadWaits();
  w[parkId] = {
    name: parkNameFor(parkId),
    wait: shortestOperatingWait((data && data.a) || []),
    ts: tsOpt == null ? Date.now() : tsOpt,
  };
  try { localStorage.setItem(WAITS_KEY, JSON.stringify(w)); } catch (e) {}
}

// Make sure each selected park has a fresh-enough wait snapshot: parks
// whose phone cache is still fresh are snapshotted from it, the rest
// get a real fetch through the normal fetchAndFilter/cache path (max
// MYPARKS_MAX = 4 requests; the fetches populate the same cache the
// watch is served from). Fetch failures fall back to the stale cache —
// buildGlanceLine's 30-minute age filter decides whether that's still
// showable. Always resolves.
function ensureMyParkWaits(parkIds) {
  var jobs = [];
  for (var i = 0; i < parkIds.length && i < lib.MYPARKS_MAX; i++) {
    (function (id) {
      var cached = loadCache(id);
      if (shouldServeCache(cached, Date.now(), STALE_THRESHOLD_MS, false)) {
        var w = loadWaits();
        // Cache is fresh but may predate this feature (or this session's
        // dest-list fetch, for the name) — refresh the snapshot from it.
        if (!w[id] || w[id].ts !== cached.timestamp || !w[id].name) {
          recordWaitSnapshot(id, cached.data, cached.timestamp);
        }
        return;
      }
      jobs.push(fetchAndFilter(id).then(function (data) {
        saveCache(id, data);
      }).catch(function (err) {
        console.log("PKJS: my-parks fetch failed for " + id + " - " + err.message);
        if (cached) recordWaitSnapshot(id, cached.data, cached.timestamp);
      }));
    })(parkIds[i]);
  }
  return Promise.all(jobs);
}

// Compose the packed line for the selected parks. Resolves to "" when
// nothing is usable (offline, all waits stale/unknown) — the caller
// then pushes an empty line, clearing the watch's stored one so it
// falls back to its locally-composed glance. Never rejects.
function buildMyParksLine(parkIds) {
  // The dest list provides display names (cached 24h) — fetch it first
  // so parkNameFor can resolve codes even on a cold start.
  return fetchDestList().catch(function () {
    return [];
  }).then(function () {
    return ensureMyParkWaits(parkIds);
  }).then(function () {
    var now = Date.now();
    var w = loadWaits();
    var entries = [];
    for (var i = 0; i < parkIds.length; i++) {
      var s = w[parkIds[i]];
      if (!s) continue;
      entries.push({
        name: s.name || parkNameFor(parkIds[i]),
        wait: s.wait,
        ageMs: now - s.ts,
      });
    }
    var line = buildGlanceLine(entries, now);
    console.log('PKJS: my-parks line "' + line + '"');
    return line;
  }).catch(function (err) {
    console.log("PKJS: my-parks line failed - " + err.message);
    return "";
  });
}

// After a park serve: if My Parks is configured, refresh the packed
// line on the watch. One tiny push per serve event.
function maybePushGlanceLine() {
  var settings = loadSettings();
  if (settings.myParks && settings.myParks.length > 0) {
    pushSettingsToWatch(settings);
  }
}

Pebble.addEventListener("ready", function () {
  console.log("PKJS ready");
  // Send an initial message so the watch marks itself as writable.
  // The watch's pebble/message module only fires onWritable after
  // receiving any message from the phone.
  const msg = {};
  msg.ERROR_MSG = "";
  Pebble.sendAppMessage(
    msg,
    function () {
      console.log("PKJS: initial ready signal sent");
      // Follow with the settings payload so the watch persist stays in
      // sync even when the user changed settings while the watch app was
      // closed (the webviewclosed push only reaches a running app).
      pushSettingsToWatch(loadSettings());
    },
    function (e) { console.log("PKJS: initial send failed: " + JSON.stringify(e)); }
  );
});

// ── Favourites sync (watch → phone) ──────────────────────────────────────
//
// The watch mirrors its favourites here after every toggle (FAVS_SYNC,
// full state per park — see lib.parseFavsSync). Persisted per park in
// localStorage FAVS_KEY as {parkUuid: {a:[...], h:[...], ts}} so
// timeline pins / ride alerts can read them without waking the watch.

var FAVS_KEY = "tpw_favs";

function handleFavsSync(raw) {
  var parsed = parseFavsSync(String(raw));
  if (!parsed) {
    console.log("PKJS: favs sync unparseable");
    return;
  }
  var prev = null;
  try { prev = JSON.parse(localStorage.getItem(FAVS_KEY)); } catch (err) {}
  var store = buildFavsStore(prev, parsed, Date.now());
  try {
    localStorage.setItem(FAVS_KEY, JSON.stringify(store));
  } catch (err) {
    console.log("PKJS: favs store save failed - " + err.message);
  }
  console.log("PKJS: favs synced " + parsed.parkId +
              " a" + parsed.attractions.length +
              " h" + parsed.shows.length);
  // Unfavourited shows lose their timeline pins (best effort): any
  // show short id present in the previous state but not the new full
  // state was just unfavourited.
  var prevShows = (prev && typeof prev === "object" && prev[parsed.parkId] &&
                   prev[parsed.parkId].h) || [];
  var removed = [];
  for (var r = 0; r < prevShows.length; r++) {
    var still = false;
    for (var n = 0; n < parsed.shows.length; n++) {
      if (parsed.shows[n] === prevShows[r]) { still = true; break; }
    }
    if (!still) removed.push(prevShows[r]);
  }
  if (removed.length > 0) deleteShowPinsFor(removed);
}

// ── Timeline pins (opt-in, default OFF) ──────────────────────────────────
//
// HARD REQUIREMENT (product owner): NOTHING is pushed to the timeline
// unless the "Timeline pins" toggle on the config page is on — pins
// default off because they could feel spammy. Turning the toggle off
// deletes previously pushed pins (best effort).
//
// Pure pin composition lives in lib.js (buildHoursPin / buildShowPin /
// selectShowPins / pinDiff — unit-tested); this section is the I/O:
// token, REST calls, bookkeeping and trigger points. Every failure
// degrades silently — a pin push must never break the serve path.
//
// Trigger points: after each sendParkData serve, and after a settings
// save with pins enabled. Per sync we push hours pins for the user's
// parks (My Parks when non-empty, else the just-served park; today +
// tomorrow, from schedules already fetched this session) and the next
// SHOW_PINS_MAX favourited showtimes of the served park — capped at
// PIN_SYNC_MAX pins total (the public API rate limit is generous, the
// user's patience is not).
//
// Bookkeeping: localStorage "tpw_pins" = {pinId: expiryMs}. Expired
// entries are pruned locally (a past pin needs no DELETE — the
// timeline ages it out). Cleanup DELETEs happen on: toggle-off (all
// ids), unfavourite (that show's ids), and per-sync for ids this sync
// is authoritative for but no longer contains (vanished showtimes).
// Pins for parks outside the current sync scope are left to expire.

var TIMELINE_API = "https://timeline-api.rebble.io";
var PINS_KEY = "tpw_pins";
var PIN_SYNC_MAX = 10;   // hard cap on pins pushed per sync
var HOURS_PIN_DAYS = 2;  // today + tomorrow
var SHOW_PIN_TTL_MS = 60 * 60 * 1000;  // show pin lingers 1h past start
var SHOW_PIN_ID_PREFIX = "tpw-show-";

// Raw pin material captured by fetchAndFilter (full entity ids + RAW
// ISO times survive here; the wire payload strips both):
//   parkId → { hours: [{date, openIso, closeIso}],
//              shows: [minimal SHOW entities], ts }
// In-memory only: a cached serve right after PKJS relaunch has no
// entry, so that sync skips the park and the next real fetch catches
// it up.
var pinSource = {};

// The park most recently served to the watch — the pin scope fallback
// when My Parks is empty ("last-used park").
var lastServedParkId = null;

// Timeline token: fetched once per session, cached. A failure disables
// pushes for the whole session (silent degrade — the service being
// unreachable must not spam retries or logs).
var timelineToken = null;
var timelineTokenFailed = false;

function withTimelineToken(cb) {
  // Test hook, mirroring tpw_test_location: the integration harness
  // seeds a fake token because the emulator's sandbox-token endpoint
  // needs a developer-portal login the CI container doesn't have.
  try {
    var fake = localStorage.getItem("tpw_test_timeline_token");
    if (fake) { cb(String(fake)); return; }
  } catch (e) {}
  if (timelineToken) { cb(timelineToken); return; }
  if (timelineTokenFailed) { cb(null); return; }
  if (typeof Pebble.getTimelineToken !== "function") {
    timelineTokenFailed = true;
    console.log("PKJS: timeline token unavailable");
    cb(null);
    return;
  }
  Pebble.getTimelineToken(
    function (token) { timelineToken = token; cb(token); },
    function () {
      timelineTokenFailed = true;
      console.log("PKJS: timeline token unavailable");
      cb(null);
    }
  );
}

function loadPinStore() {
  try {
    var raw = localStorage.getItem(PINS_KEY);
    if (raw) {
      var p = JSON.parse(raw);
      if (p && typeof p === "object") return p;
    }
  } catch (e) {}
  return {};
}

function savePinStore(store) {
  try { localStorage.setItem(PINS_KEY, JSON.stringify(store)); } catch (e) {}
}

// Generic JSON-ish XHR for the timeline REST calls. xhrGet stays
// GET-only; this one resolves {status, text} for ANY completed HTTP
// exchange (callers decide what a bad status means) and rejects only
// on network error/timeout.
function xhrJson(method, url, headers, body) {
  return new Promise(function (resolve, reject) {
    var xhr = new XMLHttpRequest();
    xhr.open(method, url, true);
    xhr.timeout = 10000;
    for (var k in headers) {
      if (Object.prototype.hasOwnProperty.call(headers, k)) {
        xhr.setRequestHeader(k, headers[k]);
      }
    }
    xhr.ontimeout = function () { reject(new Error("timeout")); };
    xhr.onload = function () { resolve({ status: xhr.status, text: xhr.responseText }); };
    xhr.onerror = function () { reject(new Error("network error")); };
    xhr.send(body == null ? null : body);
  });
}

// PUT one pin. Fire-and-forget: logged, never thrown, never awaited by
// the serve path.
function pushPin(token, pin) {
  console.log("PKJS: pin push " + pin.id);
  xhrJson("PUT", TIMELINE_API + "/v1/user/pins/" + pin.id,
          { "X-User-Token": token, "Content-Type": "application/json" },
          JSON.stringify(pin))
    .then(function (res) {
      if (res.status < 200 || res.status >= 300) {
        console.log("PKJS: pin push failed " + pin.id + " " + res.status);
      }
    })
    .catch(function (err) {
      console.log("PKJS: pin push failed " + pin.id + " " + err.message);
    });
}

// DELETE one pin. 404 is success (already gone — deletes are best
// effort and re-runnable).
function requestPinDelete(token, id) {
  console.log("PKJS: pin delete " + id);
  xhrJson("DELETE", TIMELINE_API + "/v1/user/pins/" + id,
          { "X-User-Token": token }, null)
    .then(function (res) {
      if ((res.status < 200 || res.status >= 300) && res.status !== 404) {
        console.log("PKJS: pin delete failed " + id + " " + res.status);
      }
    })
    .catch(function (err) {
      console.log("PKJS: pin delete failed " + id + " " + err.message);
    });
}

// Toggle-off cleanup: DELETE every live id we ever pushed (best
// effort), then forget them all. The store clears even when the token
// is unavailable — with pins off, stale bookkeeping is worse than a
// leaked pin that will expire on its own anyway.
function disableTimelinePins() {
  var store = loadPinStore();
  var ids = [];
  for (var id in store) {
    if (Object.prototype.hasOwnProperty.call(store, id)) ids.push(id);
  }
  if (ids.length > 0) {
    withTimelineToken(function (token) {
      if (!token) return;
      for (var i = 0; i < ids.length; i++) requestPinDelete(token, ids[i]);
    });
  }
  savePinStore({});
  console.log("PKJS: pins disabled");
}

// Unfavourite cleanup: drop every pin belonging to the given show
// short ids (id shape "tpw-show-<shortId>-<epochMins>").
function deleteShowPinsFor(shortIds) {
  var store = loadPinStore();
  var doomed = [];
  for (var id in store) {
    if (!Object.prototype.hasOwnProperty.call(store, id)) continue;
    if (id.indexOf(SHOW_PIN_ID_PREFIX) !== 0) continue;
    var sid = id.slice(SHOW_PIN_ID_PREFIX.length).split("-")[0];
    for (var s = 0; s < shortIds.length; s++) {
      if (shortIds[s] === sid) { doomed.push(id); break; }
    }
  }
  if (doomed.length === 0) return;
  for (var d = 0; d < doomed.length; d++) delete store[doomed[d]];
  savePinStore(store);
  withTimelineToken(function (token) {
    if (!token) return;
    for (var i = 0; i < doomed.length; i++) requestPinDelete(token, doomed[i]);
  });
}

// The sync entry point. Opt-in gate FIRST: with the toggle off this is
// a single settings read and out — no token fetch, no network.
function syncTimelinePins(servedParkId) {
  var settings = loadSettings();
  if (!settings.pins) return;
  withTimelineToken(function (token) {
    if (!token) return;  // degraded for the session (logged once)
    try {
      doPinSync(token, settings, servedParkId);
    } catch (e) {
      console.log("PKJS: pin sync failed - " + e.message);
    }
  });
}

function doPinSync(token, settings, servedParkId) {
  var now = Date.now();
  var store = loadPinStore();
  // Prune expired (and garbage) entries locally — no DELETE needed for
  // pins whose moment has passed.
  for (var old in store) {
    if (Object.prototype.hasOwnProperty.call(store, old) && !(store[old] > now)) {
      delete store[old];
    }
  }

  var scope = (settings.myParks && settings.myParks.length > 0)
    ? settings.myParks
    : (servedParkId ? [servedParkId] : []);

  var jobs = [];           // [{pin, expiry}]
  var hoursPrefixes = [];  // id prefixes this sync is authoritative for
  for (var i = 0; i < scope.length; i++) {
    var parkId = scope[i];
    hoursPrefixes.push("tpw-hours-" + lib.shortId(parkId) + "-");
    var src = pinSource[parkId];
    if (!src) continue;  // no schedule fetched this session — next serve
    var parkName = parkNameFor(parkId) || "Park";
    for (var h = 0; h < src.hours.length && h < HOURS_PIN_DAYS; h++) {
      var hp = lib.buildHoursPin(parkId, parkName,
                                 src.hours[h].openIso, src.hours[h].closeIso);
      if (!hp) continue;
      var closeMs = Date.parse(src.hours[h].closeIso);
      var expiry = isNaN(closeMs)
        ? Date.parse(src.hours[h].openIso) + 24 * 60 * 60 * 1000
        : closeMs;
      jobs.push({ pin: hp, expiry: expiry });
    }
  }

  // Show pins: the served park's favourited shows, next SHOW_PINS_MAX
  // future showtimes. Full ids + RAW ISO times from pinSource matched
  // against the tpw_favs short ids.
  var servedSrc = servedParkId ? pinSource[servedParkId] : null;
  var servedShowSids = {};
  if (servedSrc && servedSrc.shows.length > 0) {
    for (var e = 0; e < servedSrc.shows.length; e++) {
      servedShowSids[lib.shortId(servedSrc.shows[e].id)] = true;
    }
    var favH = [];
    try {
      var favStore = JSON.parse(localStorage.getItem(FAVS_KEY));
      if (favStore && favStore[servedParkId] && favStore[servedParkId].h) {
        favH = favStore[servedParkId].h;
      }
    } catch (err) {}
    if (favH.length > 0) {
      var servedName = parkNameFor(servedParkId) || "Park";
      var picks = lib.selectShowPins(servedSrc.shows, favH, now, lib.SHOW_PINS_MAX);
      for (var p = 0; p < picks.length; p++) {
        var sp = lib.buildShowPin(picks[p].id, picks[p].name, servedName,
                                  picks[p].startTime);
        if (sp) jobs.push({ pin: sp, expiry: picks[p].startMs + SHOW_PIN_TTL_MS });
      }
    }
  }

  if (jobs.length > PIN_SYNC_MAX) jobs = jobs.slice(0, PIN_SYNC_MAX);

  var nextIds = [];
  for (var j = 0; j < jobs.length; j++) nextIds.push(jobs[j].pin.id);

  // Cleanup diff, restricted to ids this sync is authoritative for:
  // hours pins of in-scope parks, and show pins of the served park's
  // shows (a favourited showtime that vanished upstream was cancelled
  // — its pin should go). Everything else keeps until expiry.
  var prevScoped = [];
  for (var prev in store) {
    if (!Object.prototype.hasOwnProperty.call(store, prev)) continue;
    var scoped = false;
    for (var x = 0; x < hoursPrefixes.length; x++) {
      if (prev.indexOf(hoursPrefixes[x]) === 0) { scoped = true; break; }
    }
    if (!scoped && prev.indexOf(SHOW_PIN_ID_PREFIX) === 0) {
      var sid = prev.slice(SHOW_PIN_ID_PREFIX.length).split("-")[0];
      if (servedShowSids[sid]) scoped = true;
    }
    if (scoped) prevScoped.push(prev);
  }
  var diff = lib.pinDiff(prevScoped, nextIds);

  for (var d = 0; d < diff.toDelete.length; d++) {
    delete store[diff.toDelete[d]];
    requestPinDelete(token, diff.toDelete[d]);
  }
  for (var q = 0; q < jobs.length; q++) {
    // Optimistic bookkeeping at attempt time: if the PUT fails, the
    // worst case is a later best-effort DELETE of a pin that never
    // existed (the API 404s, which requestPinDelete treats as done).
    if (jobs[q].expiry > now) store[jobs[q].pin.id] = jobs[q].expiry;
    pushPin(token, jobs[q].pin);
  }
  savePinStore(store);
}

Pebble.addEventListener("appmessage", function (e) {
  // Watch pushed its favourites after a toggle.
  var favsRaw = msgVal(e.payload, "FAVS_SYNC");
  if (favsRaw != null) {
    handleFavsSync(favsRaw);
    return;
  }

  // Multi-destination: watch requesting destination list
  if (msgVal(e.payload, "DEST_LIST_DATA") != null) {
    // Fixed-destination mode answers EVERY dest request with that
    // destination's parks, marked M|fixed — the watch then hides its
    // destination picker entirely and never re-requests the list, so
    // there is no request loop to guard against. If the fixed id no
    // longer exists upstream, handleDestSelect sends nothing and the
    // watch's dest-list watchdog re-requests — landing here again, so
    // recovery needs the setting to change (documented limitation).
    var cfg = loadSettings();
    if (IS_UBER && cfg.destMode === "fixed" && cfg.destId) {
      console.log("PKJS: fixed destination " + cfg.destId);
      handleDestSelect(cfg.destId, true);
      return;
    }
    console.log("PKJS: request for destination list");
    buildAndSendDestList();
    return;
  }

  // Multi-destination: watch selected a destination
  var destSel = msgVal(e.payload, "DEST_SELECT");
  if (destSel != null) {
    // Fixed mode also overrides the watch's startup resume (which
    // re-selects its persisted last destination): launching must always
    // land in the fixed destination, whatever the watch last browsed.
    var sel = loadSettings();
    if (IS_UBER && sel.destMode === "fixed" && sel.destId) {
      console.log("PKJS: destination select overridden by fixed " + sel.destId);
      handleDestSelect(sel.destId, true);
      return;
    }
    console.log("PKJS: destination selected: " + destSel);
    handleDestSelect(destSel, false);
    return;
  }

  // Bulk-hours request — watch wants open/close for every known park up
  // front so the picker can show them before the user opens any park.
  if (msgVal(e.payload, "REQUEST_ALL_HOURS") != null) {
    console.log("PKJS: request for all park hours");
    fetchAllHours();
    return;
  }

  const parkId = msgVal(e.payload, "REQUEST_PARK_ID");
  if (!parkId) {
    console.log("PKJS: unknown message, keys: " + Object.keys(e.payload).join(","));
    return;
  }

  // REQUEST_FORCE marks a manual "Refresh Data" — bypass the fresh-cache
  // check (the stale-fallback on fetch errors below still applies).
  var force = !!msgVal(e.payload, "REQUEST_FORCE");
  console.log("PKJS: request for park " + parkId + (force ? " (forced)" : ""));

  // Serve from cache if fresh (memory + localStorage fallback)
  const cached = loadCache(parkId);
  if (shouldServeCache(cached, Date.now(), STALE_THRESHOLD_MS, force)) {
    const ageMins = Math.floor((Date.now() - cached.timestamp) / 60000);
    console.log("PKJS: serving cached data (" + ageMins + "m old)");
    sendParkData(cached.data, parkId);
    return;
  }

  fetchAndFilter(parkId)
    .then(function (data) {
      saveCache(parkId, data);
      sendParkData(data, parkId);
    })
    .catch(function (err) {
      console.log("PKJS: error - " + err.message);
      // If fetch failed but we have a stale cached copy, serve that
      // rather than nothing.
      if (cached) {
        console.log("PKJS: falling back to stale cache");
        sendParkData(cached.data, parkId);
      } else {
        const msg = {};
        msg.ERROR_MSG = err.message;
        Pebble.sendAppMessage(msg);
      }
    });
});

// Fetch open/close for every park in one request to the destination
// schedule endpoint. The response includes a `parks` array with each
// park's full schedule — we extract today's OPERATING hours.
function fetchAllHours() {
  const todayStr = new Date().toISOString().slice(0, 10);
  // Build a Set of park IDs we care about (the destination response may
  // include water parks etc. that aren't in the watch app).
  const wantedIds = {};
  for (var i = 0; i < ALL_PARK_IDS.length; i++) {
    wantedIds[ALL_PARK_IDS[i]] = true;
  }

  xhrGet(API_BASE + "/entity/" + DESTINATION_ID + "/schedule")
    .then(function (json) {
      var parks = json.parks || [];
      var lines = [];
      for (var p = 0; p < parks.length; p++) {
        var park = parks[p];
        if (!wantedIds[park.id]) continue;
        var sc = getTodaySchedule(park.schedule || [], todayStr);
        if (sc && sc.o && sc.c) {
          lines.push(park.id + "|" + sc.o + "|" + sc.c);
        }
      }
      var payload = lines.join("\n");
      console.log("PKJS: sending all-hours (" + payload.length + " bytes, "
                  + lines.length + " parks)");
      sendChunked("ALL_HOURS_DATA", payload,
        function () { console.log("PKJS: all-hours sent ok"); },
        function () { console.log("PKJS: all-hours send failed"); }
      );
    })
    .catch(function (err) {
      console.log("PKJS: all-hours fetch failed: " + err.message);
    });
}

function sendParkData(data, parkId) {
  if (parkId) lastServedParkId = parkId;
  var payload = encodeParkData(data);
  console.log("PKJS: sending " + utf8Len(payload) + " bytes");
  sendChunked("PARK_DATA_JSON", payload,
    function () {
      console.log("PKJS: sent ok");
      // Refresh the packed My Parks glance line after every serve —
      // the serve proves the watch is listening, and the just-fetched
      // park is usually one of the selected ones.
      maybePushGlanceLine();
      // Timeline pins ride the same trigger, fire-and-forget: the
      // serve is complete, so a failed push can't cost the watch
      // anything. No-op unless the opt-in toggle is on.
      syncTimelinePins(parkId || lastServedParkId);
    },
    function (e) {
      console.log("PKJS: send failed");
      if (e) {
        for (var k in e) {
          console.log("  " + k + "=" + JSON.stringify(e[k]));
        }
      }
    }
  );
}

// PKJS uses XMLHttpRequest — not the modern fetch() API.
function xhrGet(url) {
  return new Promise(function (resolve, reject) {
    const xhr = new XMLHttpRequest();
    xhr.open("GET", url, true);
    // A hung request would otherwise leave the watch waiting out its full
    // 20s fetch watchdog — fail fast so the stale-cache fallback (or a
    // retry) takes over sooner.
    xhr.timeout = 10000;
    xhr.ontimeout = function () {
      reject(new Error("timeout"));
    };
    xhr.onload = function () {
      if (xhr.status >= 200 && xhr.status < 300) {
        try {
          resolve(JSON.parse(xhr.responseText));
        } catch (err) {
          reject(new Error("JSON parse error: " + err.message));
        }
      } else {
        reject(new Error("HTTP " + xhr.status));
      }
    };
    xhr.onerror = function () {
      reject(new Error("network error"));
    };
    xhr.send();
  });
}

function fetchAndFilter(parkId) {
  const todayStr = new Date().toISOString().slice(0, 10);

  const liveP = xhrGet(API_BASE + "/entity/" + parkId + "/live");

  const scheduleP = xhrGet(API_BASE + "/entity/" + parkId + "/schedule")
    .catch(function (err) {
      console.log("PKJS: schedule error (non-fatal) - " + err.message);
      return null;
    });

  return Promise.all([liveP, scheduleP]).then(function (results) {
    const liveJson = results[0];
    const scheduleJson = results[1];

    // Raw material for timeline pins, captured BEFORE filtering strips
    // the full entity ids and the ISO showtimes. Minimal copies only:
    // SHOW entities (id/name/showtimes.startTime) and the next
    // HOURS_PIN_DAYS operating days with their RAW ISO open/close.
    var rawShows = [];
    var rawLive = liveJson.liveData || [];
    for (var rs = 0; rs < rawLive.length; rs++) {
      var ent = rawLive[rs];
      if (!ent || ent.entityType !== "SHOW") continue;
      var sts = [];
      var entTimes = ent.showtimes || [];
      for (var st = 0; st < entTimes.length; st++) {
        if (entTimes[st] && entTimes[st].startTime) {
          sts.push({ startTime: entTimes[st].startTime });
        }
      }
      rawShows.push({ entityType: "SHOW", id: ent.id, name: ent.name, showtimes: sts });
    }
    pinSource[parkId] = {
      hours: scheduleJson
        ? lib.getUpcomingScheduleIso(scheduleJson.schedule || [], HOURS_PIN_DAYS, Date.now())
        : [],
      shows: rawShows,
      ts: Date.now(),
    };

    const filtered = filterLiveData(liveJson.liveData || []);
    const schedule = scheduleJson ? getTodaySchedule(scheduleJson.schedule || [], todayStr) : null;
    // Upcoming OPERATING days (today included — the watch dedupes: sc
    // covers today, F| entries feed the glance for dates after today).
    // 7 days ≈ the AppGlance slice budget.
    const upcoming = scheduleJson
      ? getUpcomingSchedule(scheduleJson.schedule || [], todayStr, 7)
      : [];

    // Compact payload with short field names:
    //   sc: schedule { o: open, c: close }
    //   a:  attractions [[id, name, status, waitMins], ...]
    //   h:  shows       [[id, name, status, [times]], ...]
    //   f:  upcoming days [{date, o, c}, ...]
    //   u:  lastUpdated
    // Hard cap — must match MAX_ATTRACTIONS / MAX_SHOWS in parkdata.h.
    // Operating rides sorted by wait desc, then closed.
    var a = filtered.a;
    var op = a.filter(function(x) { return x[2] === "O"; });
    var cl = a.filter(function(x) { return x[2] !== "O"; });
    op.sort(function(x, y) { return (y[3] || 0) - (x[3] || 0); });
    a = op.concat(cl).slice(0, 100);
    // Shows: keep only those with upcoming times
    var h = filtered.h.filter(function(x) { return x[3] && x[3].length > 0; });
    h = h.slice(0, 40);
    return {
      sc: schedule,
      a: a,
      h: h,
      f: upcoming,
      u: Date.now(),
    };
  });
}

// shortStatus / shortId / shortName / filterLiveData / getTodaySchedule /
// formatTime moved to lib.js (pure, unit-testable).
