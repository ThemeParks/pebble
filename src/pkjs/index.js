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
var filterLiveData = lib.filterLiveData;
var parseSettings = lib.parseSettings;
var encodeSettingsPayload = lib.encodeSettingsPayload;
var buildConfigPage = lib.buildConfigPage;

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

// Fixed-destination mode serves the fixed destination's parks in place
// of the FIRST dest-list answer of each session — see the appmessage
// handler for why it must be once-only.
var s_fixed_dest_served = false;

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

// Push the compact settings payload (see lib.encodeSettingsPayload) to
// the watch. It's a tiny single message — plain send with a couple of
// retries, no chunking needed.
function pushSettingsToWatch(settings) {
  var payload = encodeSettingsPayload(settings);
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

function openConfigPage(settings, dests) {
  var html = buildConfigPage(settings, dests, IS_UBER);
  Pebble.openURL("data:text/html;charset=utf-8," + encodeURIComponent(html));
}

Pebble.addEventListener("showConfiguration", function () {
  var settings = loadSettings();
  if (!IS_UBER) {
    openConfigPage(settings, []);
    return;
  }
  // Uber build: the destination dropdown needs the dest list (cached
  // 24h). On failure open with an empty list — the page disables the
  // fixed-destination option and shows a hint instead of never opening.
  fetchDestList().then(function (dests) {
    openConfigPage(settings, dests);
  }).catch(function (err) {
    console.log("PKJS: config dest list failed: " + err.message);
    openConfigPage(settings, []);
  });
});

Pebble.addEventListener("webviewclosed", function (e) {
  if (!e || !e.response) {
    console.log("PKJS: config page dismissed, no changes");
    return;
  }
  var raw = e.response;
  try { raw = decodeURIComponent(raw); } catch (err) {}
  // parseSettings is garbage-tolerant, so a mangled payload degrades to
  // defaults rather than throwing here.
  var settings = parseSettings(raw);
  saveSettings(settings);
  console.log("PKJS: settings saved: " + JSON.stringify(settings));
  pushSettingsToWatch(settings);
  if (IS_UBER && settings.destMode === "fixed" && settings.destId) {
    // Apply immediately: a running watch app gets the fixed
    // destination's parks pushed right away (same path as a watch-side
    // DEST_SELECT), instead of waiting for its next dest-list request.
    // This push counts as the session's one fixed serve.
    console.log("PKJS: applying fixed destination " + settings.destId);
    s_fixed_dest_served = true;
    handleDestSelect(settings.destId);
  } else {
    // Settings no longer fix a destination — re-arm so a later save
    // back to fixed mode applies within the same session.
    s_fixed_dest_served = false;
  }
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

// Watch picked a destination — update DESTINATION_ID/ALL_PARK_IDS and
// send the park list (with icon + color per park) back to the watch.
function handleDestSelect(destId) {
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

    var lines = [];
    for (var j = 0; j < dest.parks.length; j++) {
      var pk = dest.parks[j];
      lines.push(pk.id + "|" + pk.name + "|" + pk.icon + "|" + pk.color);
    }
    var payload = lines.join("\n");
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

Pebble.addEventListener("appmessage", function (e) {
  // Multi-destination: watch requesting destination list
  if (msgVal(e.payload, "DEST_LIST_DATA") != null) {
    // Fixed-destination mode answers the FIRST request of this session
    // with that destination's parks directly (DEST_PARKS_DATA) — the
    // watch then behaves like a per-destination build, and cancels its
    // dest-list watchdog on DEST_PARKS_DATA arrival (see api.c).
    //
    // Once only, because the watch re-requests the list whenever its
    // destination picker lacks data (show_picker / BACK-from-park):
    // answering every request with parks would loop forever
    // (request -> parks -> show_picker -> request), and a single-park
    // fixed destination would re-load the park on every BACK, trapping
    // the user. Later requests get the normal list, which keeps the
    // destination picker browsable — and recovers the session if the
    // fixed id no longer exists upstream (handleDestSelect sends
    // nothing, the watch's watchdog re-requests, the list answers).
    // s_fixed_dest_served resets with PKJS itself on every watch app
    // launch, and when settings change (webviewclosed re-applies).
    var cfg = loadSettings();
    if (IS_UBER && cfg.destMode === "fixed" && cfg.destId
        && !s_fixed_dest_served) {
      s_fixed_dest_served = true;
      console.log("PKJS: fixed destination " + cfg.destId);
      handleDestSelect(cfg.destId);
      return;
    }
    console.log("PKJS: request for destination list");
    buildAndSendDestList();
    return;
  }

  // Multi-destination: watch selected a destination
  var destSel = msgVal(e.payload, "DEST_SELECT");
  if (destSel != null) {
    console.log("PKJS: destination selected: " + destSel);
    handleDestSelect(destSel);
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
    sendParkData(cached.data);
    return;
  }

  fetchAndFilter(parkId)
    .then(function (data) {
      saveCache(parkId, data);
      sendParkData(data);
    })
    .catch(function (err) {
      console.log("PKJS: error - " + err.message);
      // If fetch failed but we have a stale cached copy, serve that
      // rather than nothing.
      if (cached) {
        console.log("PKJS: falling back to stale cache");
        sendParkData(cached.data);
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

function sendParkData(data) {
  var payload = encodeParkData(data);
  console.log("PKJS: sending " + utf8Len(payload) + " bytes");
  sendChunked("PARK_DATA_JSON", payload,
    function () { console.log("PKJS: sent ok"); },
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
