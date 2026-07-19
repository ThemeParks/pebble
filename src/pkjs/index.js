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

function msgVal(payload, name) {
  if (payload[name] !== undefined) return payload[name];
  return payload[MSG_KEYS[name]];
}

const API_BASE = "https://api.themeparks.wiki/v1";
const STALE_THRESHOLD_MS = 20 * 60 * 1000;
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

// In-memory cache. Persisted entries are loaded lazily on first request.
const cache = {};

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

function getParkStyle(parkId, indexInDest) {
  var style = PARK_STYLE[parkId];
  if (style) return style;
  return {
    icon: "circle",
    color: DEFAULT_PALETTE[indexInDest % DEFAULT_PALETTE.length],
  };
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

function getLocation() {
  return new Promise(function (resolve) {
    if (typeof navigator === "undefined" || !navigator.geolocation) {
      resolve(null);
      return;
    }
    navigator.geolocation.getCurrentPosition(
      function (pos) {
        resolve({ lat: pos.coords.latitude, lon: pos.coords.longitude });
      },
      function () { resolve(null); },
      { timeout: 10000, maximumAge: 60000 }
    );
  });
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

    var withDist = [];
    for (var i = 0; i < dests.length; i++) {
      var d = dests[i];
      var dist = -1;
      if (userLoc && d.lat != null && d.lon != null) {
        dist = haversineDist(userLoc.lat, userLoc.lon, d.lat, d.lon);
      }
      withDist.push({ dest: d, dist: dist });
    }

    var lines = [];

    // Nearby section (only if GPS worked)
    if (userLoc) {
      var nearby = withDist.filter(function (x) { return x.dist >= 0; });
      nearby.sort(function (a, b) { return a.dist - b.dist; });
      nearby = nearby.slice(0, 5);
      if (nearby.length > 0) {
        lines.push("H|Nearby");
        for (var n = 0; n < nearby.length; n++) {
          var nd = nearby[n];
          lines.push("D|" + nd.dest.id + "|" + nd.dest.name + "|" +
                     nd.dest.parkCount + "|" + Math.round(nd.dist));
        }
      }
    }

    // Regions
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

    var payload = lines.join("\n");
    console.log("PKJS: sending dest list (" + payload.length + " bytes, " +
                dests.length + " destinations)");
    var msg = {};
    msg.DEST_LIST_DATA = payload;
    Pebble.sendAppMessage(msg,
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
    var msg = {};
    msg.DEST_PARKS_DATA = payload;
    Pebble.sendAppMessage(msg,
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
    function () { console.log("PKJS: initial ready signal sent"); },
    function (e) { console.log("PKJS: initial send failed: " + JSON.stringify(e)); }
  );
});

Pebble.addEventListener("appmessage", function (e) {
  // Multi-destination: watch requesting destination list
  if (msgVal(e.payload, "DEST_LIST_DATA") != null) {
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

  console.log("PKJS: request for park " + parkId);

  // Serve from cache if fresh (memory + localStorage fallback)
  const cached = loadCache(parkId);
  if (cached && Date.now() - cached.timestamp < STALE_THRESHOLD_MS) {
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
  // Timestamp
  lines.push(String(data.u || Date.now()));
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
      var msg = {};
      msg.ALL_HOURS_DATA = payload;
      Pebble.sendAppMessage(
        msg,
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
  console.log("PKJS: sending " + payload.length + " bytes");
  var msg = {};
  msg.PARK_DATA_JSON = payload;
  Pebble.sendAppMessage(
    msg,
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

    // Compact payload with short field names:
    //   sc: schedule { o: open, c: close }
    //   a:  attractions [[id, name, status, waitMins], ...]
    //   h:  shows       [[id, name, status, [times]], ...]
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
      u: Date.now(),
    };
  });
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

function filterLiveData(liveData) {
  const attractions = [];
  const shows = [];

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
        const now = Date.now();
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

function getTodaySchedule(schedule, todayStr) {
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
  var now = Date.now();
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

function formatTime(isoString) {
  // Extract HH:MM from ISO string — the timezone offset in the string
  // already reflects the park's local time
  const match = isoString.match(/T(\d{2}):(\d{2})/);
  if (!match) return "";
  let hours = parseInt(match[1], 10);
  const minutes = parseInt(match[2], 10);
  const ampm = hours >= 12 ? "PM" : "AM";
  hours = hours % 12 || 12;
  const min = minutes > 0 ? ":" + (minutes < 10 ? "0" + minutes : minutes) : "";
  return hours + min + " " + ampm;
}
