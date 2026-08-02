// test_lib.js — unit tests for src/pkjs/lib.js (the pure helpers).
//
// Plain node script, no framework: `node tests/js/test_lib.js`.
// Each test() runs immediately; failures print and set a non-zero exit
// code via process.exitCode so the runner (scripts/unit_tests.py) and
// CI both see them.

var assert = require("assert");
var path = require("path");
var lib = require(path.join(__dirname, "..", "..", "src", "pkjs", "lib.js"));

var passed = 0;
var failed = 0;

function test(name, fn) {
  try {
    fn();
    passed++;
    console.log("PASS " + name);
  } catch (err) {
    failed++;
    console.log("FAIL " + name + ": " + err.message);
  }
}

// ── utf8Len ──────────────────────────────────────────────────────────────

test("utf8Len ascii", function () {
  assert.strictEqual(lib.utf8Len(""), 0);
  assert.strictEqual(lib.utf8Len("hello"), 5);
});

test("utf8Len 2-byte", function () {
  // U+00E9 (é) encodes to 2 bytes
  assert.strictEqual(lib.utf8Len("café"), 5);
});

test("utf8Len 3-byte", function () {
  // U+20AC (€) encodes to 3 bytes
  assert.strictEqual(lib.utf8Len("€9"), 4);
});

test("utf8Len surrogate pair", function () {
  // U+1F600 (😀) is a surrogate pair in UTF-16, 4 bytes in UTF-8
  assert.strictEqual(lib.utf8Len("😀"), 4);
  assert.strictEqual(lib.utf8Len("a😀b"), 6);
});

// ── splitIntoChunks ──────────────────────────────────────────────────────

test("splitIntoChunks empty payload", function () {
  assert.deepStrictEqual(lib.splitIntoChunks("", 100), [""]);
});

test("splitIntoChunks single small line", function () {
  assert.deepStrictEqual(lib.splitIntoChunks("hello", 100), ["hello"]);
});

test("splitIntoChunks exact-boundary packing", function () {
  // "aaaa\nbbbbb" is exactly 10 bytes — fits a limit of 10 in one chunk...
  assert.deepStrictEqual(lib.splitIntoChunks("aaaa\nbbbbb", 10), ["aaaa\nbbbbb"]);
  // ...but one byte over the limit splits, with the joining '\n' elided.
  assert.deepStrictEqual(lib.splitIntoChunks("aaaa\nbbbbb", 9), ["aaaa", "bbbbb"]);
});

test("splitIntoChunks oversize single line is its own chunk", function () {
  var big = new Array(21).join("x"); // 20 x's
  var chunks = lib.splitIntoChunks("short\n" + big + "\nend", 10);
  assert.deepStrictEqual(chunks, ["short", big, "end"]);
});

test("splitIntoChunks joined chunks reproduce payload", function () {
  // Multi-chunk: rejoining with '\n' must reproduce the original,
  // including an interior blank line.
  var lines = [];
  for (var i = 0; i < 30; i++) lines.push("line-number-" + i);
  lines.splice(10, 0, "");
  var payload = lines.join("\n");
  var chunks = lib.splitIntoChunks(payload, 50);
  assert.ok(chunks.length > 1, "expected multiple chunks");
  for (var c = 0; c < chunks.length; c++) {
    assert.ok(lib.utf8Len(chunks[c]) <= 50, "chunk " + c + " over limit");
  }
  assert.strictEqual(chunks.join("\n"), payload);
});

// ── encodeParkData ───────────────────────────────────────────────────────

test("encodeParkData with schedule", function () {
  var out = lib.encodeParkData({
    sc: { o: "9 AM", c: "10 PM" },
    u: 1753776000000,   // ms in, seconds out on the wire
    a: [],
    h: [],
  });
  assert.strictEqual(out, "9 AM|10 PM\n1753776000");
});

test("encodeParkData without schedule", function () {
  var out = lib.encodeParkData({ sc: null, u: 42, a: [], h: [] });
  assert.strictEqual(out.split("\n")[0], "");
});

test("encodeParkData timestamp converts ms to seconds", function () {
  // The watch parses line 2 into a 32-bit seconds value, so the encoder
  // must divide the millisecond timestamp down (flooring sub-second
  // remainders).
  var out = lib.encodeParkData({ sc: null, u: 1753776000123, a: [], h: [] });
  assert.strictEqual(out.split("\n")[1], "1753776000");
});

test("encodeParkData timestamp fallback is seconds-scale", function () {
  // With u missing the encoder stamps 'now' — which must also be
  // seconds (10-ish digits), not milliseconds (13 digits).
  var out = lib.encodeParkData({ sc: null, a: [], h: [] });
  var ts = Number(out.split("\n")[1]);
  assert.ok(ts > 1e9, "timestamp too small: " + ts);
  assert.ok(ts < 1e11, "timestamp looks like milliseconds: " + ts);
});

test("encodeParkData attractions", function () {
  var out = lib.encodeParkData({
    sc: null,
    u: 1,
    a: [
      ["abcd1234", "Space Mountain", "O", 45],
      ["ef567890", "Haunted Mansion", "C", null],  // null wait → empty field
      ["00000000", "Zero Wait", "O", 0],
    ],
    h: [],
  });
  var lines = out.split("\n");
  assert.strictEqual(lines[2], "A|abcd1234|Space Mountain|O|45");
  assert.strictEqual(lines[3], "A|ef567890|Haunted Mansion|C|");
  assert.strictEqual(lines[4], "A|00000000|Zero Wait|O|0");
});

test("encodeParkData shows", function () {
  var out = lib.encodeParkData({
    sc: null,
    u: 1,
    a: [],
    h: [
      ["show0001", "Fireworks", "O", ["9:30 PM", "10:30 PM"]],
      ["show0002", "No Times", "C", null],
    ],
  });
  var lines = out.split("\n");
  assert.strictEqual(lines[2], "S|show0001|Fireworks|O|9:30 PM,10:30 PM");
  assert.strictEqual(lines[3], "S|show0002|No Times|C|");
});

// ── shortStatus / shortId / shortName ────────────────────────────────────

test("shortStatus mapping", function () {
  assert.strictEqual(lib.shortStatus("OPERATING"), "O");
  assert.strictEqual(lib.shortStatus("CLOSED"), "C");
  assert.strictEqual(lib.shortStatus("DOWN"), "D");
  assert.strictEqual(lib.shortStatus("REFURBISHMENT"), "R");
  assert.strictEqual(lib.shortStatus("SOMETHING_ELSE"), "?");
  assert.strictEqual(lib.shortStatus(undefined), "?");
});

test("shortId strips dashes, keeps last 8", function () {
  assert.strictEqual(lib.shortId("75ea578a-adc8-4116-a54d-dccb60765ef9"), "60765ef9");
  assert.strictEqual(lib.shortId("abc"), "abc");
  assert.strictEqual(lib.shortId(null), "");
  assert.strictEqual(lib.shortId(""), "");
});

test("shortName 36-char cap", function () {
  var name34 = "The Twilight Zone Tower of Terror!";
  assert.strictEqual(lib.shortName(name34), name34);
  var name36 = new Array(37).join("x");
  assert.strictEqual(lib.shortName(name36), name36);
  var name40 = new Array(41).join("y");
  var out = lib.shortName(name40);
  assert.strictEqual(out.length, 36);
  assert.strictEqual(out, name40.slice(0, 35) + ".");
  assert.strictEqual(lib.shortName(null), null);
});

// ── formatTime ───────────────────────────────────────────────────────────
// Wire format is zero-padded 24h "HH:MM" — the park's local wall clock,
// straight from the ISO string. Display formatting (12/24h) happens on
// the watch (format.c), tested by tests/unit/test_format.c.

test("formatTime extracts 24h wall clock", function () {
  assert.strictEqual(lib.formatTime("2026-07-29T21:30:00-04:00"), "21:30");
  assert.strictEqual(lib.formatTime("2026-07-29T09:05:00-04:00"), "09:05");
});

test("formatTime keeps zero padding", function () {
  assert.strictEqual(lib.formatTime("2026-07-29T21:00:00-04:00"), "21:00");
  assert.strictEqual(lib.formatTime("2026-07-29T00:05:00Z"), "00:05");
  assert.strictEqual(lib.formatTime("2026-07-29T12:00:00Z"), "12:00");
});

test("formatTime bad string", function () {
  assert.strictEqual(lib.formatTime("not a time"), "");
});

// ── shouldServeCache ─────────────────────────────────────────────────────

test("shouldServeCache fresh cache is served", function () {
  var cached = { timestamp: 1000000, data: {} };
  assert.strictEqual(lib.shouldServeCache(cached, 1000000 + 60000, 240000, false), true);
});

test("shouldServeCache stale cache is not served", function () {
  var cached = { timestamp: 1000000, data: {} };
  assert.strictEqual(lib.shouldServeCache(cached, 1000000 + 240000, 240000, false), false);
});

test("shouldServeCache missing cache is not served", function () {
  assert.strictEqual(lib.shouldServeCache(null, 1000000, 240000, false), false);
  assert.strictEqual(lib.shouldServeCache(undefined, 1000000, 240000, false), false);
});

test("shouldServeCache force bypasses even a fresh cache", function () {
  var cached = { timestamp: 1000000, data: {} };
  assert.strictEqual(lib.shouldServeCache(cached, 1000000 + 1000, 240000, true), false);
});

// ── getTodaySchedule ─────────────────────────────────────────────────────

var SCHED = [
  { date: "2026-07-28", type: "OPERATING",
    openingTime: "2026-07-28T09:00:00-04:00", closingTime: "2026-07-28T22:00:00-04:00" },
  { date: "2026-07-29", type: "TICKETED_EVENT",
    openingTime: "2026-07-29T07:00:00-04:00", closingTime: "2026-07-29T09:00:00-04:00" },
  { date: "2026-07-29", type: "OPERATING",
    openingTime: "2026-07-29T09:00:00-04:00", closingTime: "2026-07-29T22:00:00-04:00" },
];

test("getTodaySchedule exact date match", function () {
  // nowMs pinned way in the past — the exact-match path must win and
  // skip the non-OPERATING entry for the same date.
  var sc = lib.getTodaySchedule(SCHED, "2026-07-29", 0);
  assert.deepStrictEqual(sc, { o: "09:00", c: "22:00" });
});

test("getTodaySchedule falls back to next operating session", function () {
  // No entry for "today" (park east of UTC rolled over) — should return
  // the session still open relative to pinned now.
  var nowMs = new Date("2026-07-29T20:00:00-04:00").getTime();
  var sc = lib.getTodaySchedule(SCHED, "2026-07-30", nowMs);
  assert.deepStrictEqual(sc, { o: "09:00", c: "22:00" });
});

test("getTodaySchedule null when nothing matches", function () {
  assert.strictEqual(lib.getTodaySchedule([], "2026-07-29", 0), null);
  // All sessions already closed relative to pinned now.
  var nowMs = new Date("2026-08-01T00:00:00Z").getTime();
  assert.strictEqual(lib.getTodaySchedule(SCHED, "2026-08-01", nowMs), null);
});

// ── getUpcomingSchedule ──────────────────────────────────────────────────

var UPCOMING_SCHED = [
  // Out of order on purpose — the helper must sort by date.
  { date: "2026-07-31", type: "OPERATING",
    openingTime: "2026-07-31T08:30:00-04:00", closingTime: "2026-08-01T01:00:00-04:00" },
  { date: "2026-07-28", type: "OPERATING",
    openingTime: "2026-07-28T09:00:00-04:00", closingTime: "2026-07-28T22:00:00-04:00" },
  { date: "2026-07-29", type: "TICKETED_EVENT",
    openingTime: "2026-07-29T07:00:00-04:00", closingTime: "2026-07-29T09:00:00-04:00" },
  { date: "2026-07-29", type: "OPERATING",
    openingTime: "2026-07-29T09:00:00-04:00", closingTime: "2026-07-29T22:00:00-04:00" },
  { date: "2026-07-30", type: "OPERATING",
    openingTime: "2026-07-30T09:00:00-04:00", closingTime: "2026-07-30T22:00:00-04:00" },
  // Duplicate OPERATING entry for a date — first (after sort) wins.
  { date: "2026-07-30", type: "OPERATING",
    openingTime: "2026-07-30T10:00:00-04:00", closingTime: "2026-07-30T23:00:00-04:00" },
  { date: "2026-08-01", type: "INFO" },  // non-OPERATING, no times
];

test("getUpcomingSchedule from today onward, ordered, deduped", function () {
  // Pinned midday on the 29th: the 28th is fully past (dropped), the
  // 29th's session is still open (kept), then the 30th and 31st.
  var nowMs = new Date("2026-07-29T12:00:00-04:00").getTime();
  var got = lib.getUpcomingSchedule(UPCOMING_SCHED, "2026-07-29", 7, nowMs);
  assert.deepStrictEqual(got, [
    { date: "2026-07-29", o: "09:00", c: "22:00" },
    { date: "2026-07-30", o: "09:00", c: "22:00" },
    { date: "2026-07-31", o: "08:30", c: "01:00" },
  ]);
});

test("getUpcomingSchedule maxDays cap", function () {
  var nowMs = new Date("2026-07-29T12:00:00-04:00").getTime();
  var got = lib.getUpcomingSchedule(UPCOMING_SCHED, "2026-07-29", 2, nowMs);
  assert.strictEqual(got.length, 2);
  assert.strictEqual(got[0].date, "2026-07-29");
  assert.strictEqual(got[1].date, "2026-07-30");
});

test("getUpcomingSchedule keeps a still-open overnight session", function () {
  // Park east of UTC thinking: local "today" (the 1st) has rolled past
  // the schedule's last date, but the 31st's session closes at 1 AM on
  // the 1st and is still open — it must be kept.
  var nowMs = new Date("2026-08-01T00:30:00-04:00").getTime();
  var got = lib.getUpcomingSchedule(UPCOMING_SCHED, "2026-08-01", 7, nowMs);
  assert.deepStrictEqual(got, [
    { date: "2026-07-31", o: "08:30", c: "01:00" },
  ]);
});

test("getUpcomingScheduleIso keeps raw ISO strings", function () {
  // Same selection as getUpcomingSchedule but with the offset-bearing
  // ISO strings intact — timeline pins are built from these.
  var nowMs = new Date("2026-07-29T12:00:00-04:00").getTime();
  var got = lib.getUpcomingScheduleIso(UPCOMING_SCHED, 2, nowMs);
  assert.deepStrictEqual(got, [
    { date: "2026-07-29",
      openIso: "2026-07-29T09:00:00-04:00",
      closeIso: "2026-07-29T22:00:00-04:00" },
    { date: "2026-07-30",
      openIso: "2026-07-30T09:00:00-04:00",
      closeIso: "2026-07-30T22:00:00-04:00" },
  ]);
  assert.deepStrictEqual(lib.getUpcomingScheduleIso([], 7, 0), []);
});

test("getUpcomingSchedule empty when nothing upcoming", function () {
  var nowMs = new Date("2026-08-02T12:00:00Z").getTime();
  assert.deepStrictEqual(
    lib.getUpcomingSchedule(UPCOMING_SCHED, "2026-08-02", 7, nowMs), []);
  assert.deepStrictEqual(lib.getUpcomingSchedule([], "2026-07-29", 7, 0), []);
});

// ── encodeParkData F| lines ──────────────────────────────────────────────

test("encodeParkData emits F| lines from data.f", function () {
  var out = lib.encodeParkData({
    sc: { o: "09:00", c: "22:00" },
    u: 1753776000000,
    a: [["abcd1234", "Space Mountain", "O", 45]],
    h: [],
    f: [
      { date: "2026-07-29", o: "09:00", c: "22:00" },
      { date: "2026-07-30", o: "08:30", c: "01:00" },
    ],
  });
  var lines = out.split("\n");
  assert.strictEqual(lines[2], "A|abcd1234|Space Mountain|O|45");
  assert.strictEqual(lines[3], "F|2026-07-29|09:00|22:00");
  assert.strictEqual(lines[4], "F|2026-07-30|08:30|01:00");
  assert.strictEqual(lines.length, 5);
});

test("encodeParkData without f is byte-identical to the old shape", function () {
  // Absent and empty f must both emit nothing — old payloads (and the
  // watch's pre-F parser) see exactly the same bytes as before.
  var base = { sc: { o: "9 AM", c: "10 PM" }, u: 1753776000000, a: [], h: [] };
  var withoutF = lib.encodeParkData(base);
  assert.strictEqual(withoutF, "9 AM|10 PM\n1753776000");
  base.f = [];
  assert.strictEqual(lib.encodeParkData(base), withoutF);
});

// ── classifyRegion ───────────────────────────────────────────────────────

test("classifyRegion sample points", function () {
  assert.strictEqual(lib.classifyRegion(28.4, -81.6), "North America");   // Orlando
  assert.strictEqual(lib.classifyRegion(48.9, 2.35), "Europe");           // Paris
  assert.strictEqual(lib.classifyRegion(35.6, 139.7), "Asia Pacific");    // Tokyo
  assert.strictEqual(lib.classifyRegion(-23.5, -46.6), "Latin America");  // São Paulo
  assert.strictEqual(lib.classifyRegion(25.2, 55.3), "Middle East & Africa"); // Dubai
});

// ── haversineDist ────────────────────────────────────────────────────────

test("haversineDist London-Paris within 1%", function () {
  // Great-circle London (51.5074, -0.1278) → Paris (48.8566, 2.3522)
  // is ~343.5 km.
  var d = lib.haversineDist(51.5074, -0.1278, 48.8566, 2.3522);
  assert.ok(Math.abs(d - 343.5) < 3.5, "got " + d);
  // Zero distance sanity check.
  assert.strictEqual(lib.haversineDist(10, 20, 10, 20), 0);
});

// ── buildDestLines ───────────────────────────────────────────────────────

var DEST_FIXTURE = [
  { id: "wdw", name: "Walt Disney World", parkCount: 4, lat: 28.4, lon: -81.6 },
  { id: "dlp", name: "Disneyland Paris", parkCount: 2, lat: 48.9, lon: 2.8 },
  { id: "adv", name: "Adventureland", parkCount: 1, lat: 41.6, lon: -93.5 },
  { id: "nol", name: "No Location Dest", parkCount: 1, lat: null, lon: null },
];

test("buildDestLines with location: Nearby first, sorted by distance", function () {
  var lines = lib.buildDestLines(DEST_FIXTURE, { lat: 28.4, lon: -81.6 });
  assert.strictEqual(lines[0], "H|Nearby");
  // Nearest is WDW at ~0 km.
  assert.strictEqual(lines[1], "D|wdw|Walt Disney World|4|0");
  // All located destinations appear in Nearby (only 3 here, cap is 10).
  assert.strictEqual(lines[2].slice(0, 6), "D|adv|");
  assert.strictEqual(lines[3].slice(0, 6), "D|dlp|");
  // No X| marker anywhere.
  assert.ok(lines.every(function (l) { return l.slice(0, 2) !== "X|"; }));
  // Region sections follow, alphabetically: Europe before North America.
  var europe = lines.indexOf("H|Europe");
  var na = lines.indexOf("H|North America");
  assert.ok(europe > 0 && na > europe);
  // The location-less destination is absent entirely.
  assert.ok(lines.every(function (l) { return l.indexOf("|nol|") === -1; }));
});

test("buildDestLines without location: X marker, no Nearby", function () {
  var lines = lib.buildDestLines(DEST_FIXTURE, null);
  assert.strictEqual(lines[0], "X|nogps");
  assert.strictEqual(lines.indexOf("H|Nearby"), -1);
  // Regions still present, region rows carry dist 0.
  assert.ok(lines.indexOf("H|Europe") >= 0);
  assert.ok(lines.indexOf("D|dlp|Disneyland Paris|2|0") >= 0);
});

test("buildDestLines region destinations sorted by name", function () {
  var lines = lib.buildDestLines(DEST_FIXTURE, null);
  var na = lines.indexOf("H|North America");
  // Adventureland alphabetically precedes Walt Disney World.
  assert.strictEqual(lines[na + 1].slice(0, 6), "D|adv|");
  assert.strictEqual(lines[na + 2].slice(0, 6), "D|wdw|");
});

// ── buildParkLines ───────────────────────────────────────────────────────

var PARKS_FIXTURE = [
  { id: "p1", name: "Efteling", icon: "castle", color: "purple" },
  { id: "p2", name: "Second Park", icon: "circle", color: "blue" },
];

test("buildParkLines plain payload has no marker", function () {
  var lines = lib.buildParkLines(PARKS_FIXTURE, false);
  assert.deepStrictEqual(lines, [
    "p1|Efteling|castle|purple",
    "p2|Second Park|circle|blue",
  ]);
});

test("buildParkLines fixed payload leads with M|fixed", function () {
  var lines = lib.buildParkLines(PARKS_FIXTURE, true);
  assert.strictEqual(lines[0], "M|fixed");
  assert.strictEqual(lines.length, 3);
  assert.strictEqual(lines[1], "p1|Efteling|castle|purple");
});

test("buildParkLines empty parks", function () {
  assert.deepStrictEqual(lib.buildParkLines([], false), []);
  assert.deepStrictEqual(lib.buildParkLines([], true), ["M|fixed"]);
});

// ── filterLiveData ───────────────────────────────────────────────────────

test("filterLiveData attractions", function () {
  var nowMs = new Date("2026-07-29T12:00:00Z").getTime();
  var out = lib.filterLiveData([
    { entityType: "ATTRACTION", id: "aaaaaaaa-bbbb-cccc-dddd-eeeeffff0001",
      name: "Coaster", status: "OPERATING",
      queue: { STANDBY: { waitTime: 30 } } },
    { entityType: "ATTRACTION", id: "aaaaaaaa-bbbb-cccc-dddd-eeeeffff0002",
      name: "Flume", status: "DOWN" },                       // no queue
    { entityType: "ATTRACTION", id: "aaaaaaaa-bbbb-cccc-dddd-eeeeffff0003",
      name: "Spinner", status: "OPERATING",
      queue: { STANDBY: { waitTime: null } } },              // null waitTime
    { entityType: "RESTAURANT", id: "x", name: "Food", status: "OPERATING" },
  ], nowMs);

  assert.strictEqual(out.a.length, 3);
  assert.deepStrictEqual(out.a[0], ["ffff0001", "Coaster", "O", 30]);
  assert.strictEqual(out.a[1][3], null);
  assert.strictEqual(out.a[2][3], null);
  assert.strictEqual(out.h.length, 0);
});

test("filterLiveData shows keep only future times, max 4", function () {
  var nowMs = new Date("2026-07-29T12:00:00Z").getTime();
  var out = lib.filterLiveData([
    { entityType: "SHOW", id: "aaaaaaaa-bbbb-cccc-dddd-eeeeffff0004",
      name: "Parade", status: "OPERATING",
      showtimes: [
        { startTime: "2026-07-29T10:00:00Z" },  // past — dropped
        { startTime: "2026-07-29T13:00:00Z" },
        { startTime: "2026-07-29T14:00:00Z" },
        { startTime: "2026-07-29T15:00:00Z" },
        { startTime: "2026-07-29T16:00:00Z" },
        { startTime: "2026-07-29T17:00:00Z" },  // 5th future — capped
      ] },
    { entityType: "SHOW", id: "aaaaaaaa-bbbb-cccc-dddd-eeeeffff0005",
      name: "Done Show", status: "CLOSED" },     // no showtimes at all
  ], nowMs);

  assert.strictEqual(out.h.length, 2);
  assert.deepStrictEqual(out.h[0],
    ["ffff0004", "Parade", "O", ["13:00", "14:00", "15:00", "16:00"]]);
  assert.deepStrictEqual(out.h[1][3], []);
});

// ── parseSettings / defaultSettings ──────────────────────────────────────

test("defaultSettings shape", function () {
  assert.deepStrictEqual(lib.defaultSettings(),
    { v: 1, glance: true, destMode: "watch", destId: null, myParks: [],
      pins: false });
});

test("parseSettings garbage yields defaults", function () {
  var def = lib.defaultSettings();
  assert.deepStrictEqual(lib.parseSettings(null), def);
  assert.deepStrictEqual(lib.parseSettings(undefined), def);
  assert.deepStrictEqual(lib.parseSettings(""), def);
  assert.deepStrictEqual(lib.parseSettings("not json {"), def);
  assert.deepStrictEqual(lib.parseSettings("42"), def);
  assert.deepStrictEqual(lib.parseSettings("null"), def);
  assert.deepStrictEqual(lib.parseSettings('"a string"'), def);
});

test("parseSettings full round-trip", function () {
  var s = { v: 1, glance: false, destMode: "fixed", destId: "dest-wdw",
            myParks: ["park-a", "park-b"], pins: true };
  assert.deepStrictEqual(lib.parseSettings(JSON.stringify(s)), s);
});

test("parseSettings partials get defaults", function () {
  var got = lib.parseSettings('{"glance":false}');
  assert.deepStrictEqual(got,
    { v: 1, glance: false, destMode: "watch", destId: null, myParks: [],
      pins: false });
});

test("parseSettings pins defaults false and stays opt-in", function () {
  // Absent → false (the owner requirement: never opted in by accident).
  assert.strictEqual(lib.parseSettings(null).pins, false);
  assert.strictEqual(lib.parseSettings("{}").pins, false);
  // Explicit booleans round-trip.
  assert.strictEqual(lib.parseSettings('{"pins":true}').pins, true);
  assert.strictEqual(lib.parseSettings('{"pins":false}').pins, false);
  // Anything mistyped degrades to OFF, never on.
  assert.strictEqual(lib.parseSettings('{"pins":"true"}').pins, false);
  assert.strictEqual(lib.parseSettings('{"pins":1}').pins, false);
  assert.strictEqual(lib.parseSettings('{"pins":{}}').pins, false);
});

test("parseSettings mistyped fields fall back", function () {
  var got = lib.parseSettings(
    '{"glance":"yes","destMode":"banana","destId":123,"v":"x","myParks":"a"}');
  assert.deepStrictEqual(got, lib.defaultSettings());
});

test("parseSettings unknown fields dropped", function () {
  var got = lib.parseSettings('{"glance":false,"futureThing":["a","b"],"zzz":1}');
  assert.strictEqual(got.futureThing, undefined);
  assert.strictEqual(got.zzz, undefined);
  assert.strictEqual(got.glance, false);
});

test("parseSettings myParks absent yields empty array", function () {
  assert.deepStrictEqual(lib.parseSettings('{"glance":true}').myParks, []);
  assert.deepStrictEqual(lib.parseSettings(null).myParks, []);
});

test("parseSettings myParks junk entries dropped", function () {
  // Non-strings, empty strings and nested junk are dropped; the good
  // entries survive in order.
  var got = lib.parseSettings(
    '{"myParks":[42,"park-a",null,"",["x"],{"a":1},"park-b",true]}');
  assert.deepStrictEqual(got.myParks, ["park-a", "park-b"]);
  // Non-array myParks is junk wholesale.
  assert.deepStrictEqual(lib.parseSettings('{"myParks":"park-a"}').myParks, []);
  assert.deepStrictEqual(lib.parseSettings('{"myParks":{"0":"a"}}').myParks, []);
});

test("parseSettings myParks over-cap truncated to 4", function () {
  var got = lib.parseSettings('{"myParks":["a","b","c","d","e","f"]}');
  assert.deepStrictEqual(got.myParks, ["a", "b", "c", "d"]);
  assert.strictEqual(lib.MYPARKS_MAX, 4);
});

test("parseSettings myParks duplicates collapse", function () {
  var got = lib.parseSettings('{"myParks":["a","a","b","a","b"]}');
  assert.deepStrictEqual(got.myParks, ["a", "b"]);
});

test("parseSettings version passthrough", function () {
  // A newer schema version must survive parse untouched so a future
  // migration can see what it's migrating from.
  assert.strictEqual(lib.parseSettings('{"v":2}').v, 2);
});

test("parseSettings fixed without destId degrades to watch", function () {
  var got = lib.parseSettings('{"destMode":"fixed"}');
  assert.strictEqual(got.destMode, "watch");
  assert.strictEqual(got.destId, null);
  got = lib.parseSettings('{"destMode":"fixed","destId":""}');
  assert.strictEqual(got.destMode, "watch");
});

// ── encodeSettingsPayload ────────────────────────────────────────────────

test("encodeSettingsPayload both glance states", function () {
  assert.strictEqual(
    lib.encodeSettingsPayload(lib.defaultSettings()), "glance=1");
  var s = lib.defaultSettings();
  s.glance = false;
  assert.strictEqual(lib.encodeSettingsPayload(s), "glance=0");
});

test("encodeSettingsPayload appends the glance line segment", function () {
  assert.strictEqual(
    lib.encodeSettingsPayload(lib.defaultSettings(),
                              "MK 45m · EP 30m", 1754081234),
    "glance=1;line=MK 45m · EP 30m|1754081234");
  // Empty text is the explicit clear.
  assert.strictEqual(
    lib.encodeSettingsPayload(lib.defaultSettings(), "", 0),
    "glance=1;line=|0");
  // null lineText → no segment at all (byte-identical to the old wire).
  assert.strictEqual(
    lib.encodeSettingsPayload(lib.defaultSettings(), null, 123),
    "glance=1");
});

// ── parkCode ─────────────────────────────────────────────────────────────

test("parkCode initials of capitalised words", function () {
  assert.strictEqual(lib.parkCode("Magic Kingdom"), "MK");
  assert.strictEqual(lib.parkCode("Epic Universe"), "EU");
  assert.strictEqual(lib.parkCode("Disney's Hollywood Studios"), "DHS");
});

test("parkCode skips lowercase connectives", function () {
  assert.strictEqual(lib.parkCode("Islands of Adventure"), "IA");
  assert.strictEqual(lib.parkCode("Legoland at the Beach"), "LB");
});

test("parkCode counts camelCase inner capitals", function () {
  // Device-found bug: Tokyo Disneyland and Tokyo DisneySea both read
  // "TD" without the inner-capital rule.
  assert.strictEqual(lib.parkCode("Tokyo DisneySea"), "TDS");
  assert.strictEqual(lib.parkCode("Tokyo Disneyland"), "TD");
});

test("assignParkCodes disambiguates collisions", function () {
  // Natural non-collision via camelCase:
  assert.deepStrictEqual(
    lib.assignParkCodes(["Tokyo Disneyland", "Tokyo DisneySea"]),
    ["TD", "TDS"]);
  // Forced collision: same initials, no camelCase help — the first
  // differing flattened character disambiguates.
  var codes = lib.assignParkCodes(["Magic Kingdom", "Magic Konda"]);
  assert.notStrictEqual(codes[0], codes[1]);
  assert.strictEqual(codes[0].slice(0, 2), "MK");
  assert.strictEqual(codes[1].slice(0, 2), "MK");
  // Non-colliding sets pass through untouched.
  assert.deepStrictEqual(
    lib.assignParkCodes(["Magic Kingdom", "Epic Universe"]),
    ["MK", "EU"]);
  // Truly identical names keep the shared code — nothing to tell apart.
  assert.deepStrictEqual(
    lib.assignParkCodes(["Legoland", "Legoland"]),
    ["LEG", "LEG"]);
});

test("buildGlanceLine uses disambiguated codes", function () {
  var line = lib.buildGlanceLine([
    { name: "Tokyo Disneyland", wait: 5, ageMs: 1000 },
    { name: "Tokyo DisneySea", wait: 7, ageMs: 1000 },
  ], 0);
  assert.strictEqual(line, "TD 5m · TDS 7m");
});

test("parkCode caps at 3 characters", function () {
  assert.strictEqual(lib.parkCode("Disney's Animal Kingdom Theme Park"), "DAK");
  assert.strictEqual(lib.parkCode("One Two Three Four Five"), "OTT");
});

test("parkCode single-word names take first 3 letters uppercased", function () {
  assert.strictEqual(lib.parkCode("EPCOT"), "EPC");
  assert.strictEqual(lib.parkCode("Efteling"), "EFT");
  assert.strictEqual(lib.parkCode("waterpark"), "WAT");
  // Short single words survive as-is.
  assert.strictEqual(lib.parkCode("Zoo"), "ZOO");
  assert.strictEqual(lib.parkCode("Q"), "Q");
});

test("parkCode digit-led words count as initials", function () {
  assert.strictEqual(lib.parkCode("2nd Gate Park"), "2GP");
});

test("parkCode degenerate inputs", function () {
  assert.strictEqual(lib.parkCode(""), "");
  assert.strictEqual(lib.parkCode(null), "");
  assert.strictEqual(lib.parkCode(undefined), "");
  // Punctuation-only name has nothing usable.
  assert.strictEqual(lib.parkCode(";;; ==="), "");
  // Punctuation-led words contribute no initials; the fallback slice
  // sees only alphanumerics, so the charset stays [A-Z0-9].
  assert.strictEqual(lib.parkCode("=Bad ;Name"), "BAD");
});

// ── shortestOperatingWait ────────────────────────────────────────────────

test("shortestOperatingWait picks min among operating", function () {
  var a = [
    ["id1", "Coaster", "O", 45],
    ["id2", "Flume", "O", 10],
    ["id3", "Spinner", "O", 30],
  ];
  assert.strictEqual(lib.shortestOperatingWait(a), 10);
});

test("shortestOperatingWait ignores closed and unknown waits", function () {
  var a = [
    ["id1", "Closed Short", "C", 0],     // closed — ignored
    ["id2", "Down", "D", 5],             // down — ignored
    ["id3", "No Wait", "O", null],       // operating, unknown wait
    ["id4", "Real", "O", 25],
  ];
  assert.strictEqual(lib.shortestOperatingWait(a), 25);
  // Zero is a real wait, not "missing".
  a.push(["id5", "Walk On", "O", 0]);
  assert.strictEqual(lib.shortestOperatingWait(a), 0);
});

test("shortestOperatingWait null when nothing usable", function () {
  assert.strictEqual(lib.shortestOperatingWait([]), null);
  assert.strictEqual(
    lib.shortestOperatingWait([["id1", "X", "C", 5], ["id2", "Y", "O", null]]),
    null);
});

// ── buildGlanceLine ──────────────────────────────────────────────────────

var NOW = 1754000000000;

test("buildGlanceLine composes in given order", function () {
  var line = lib.buildGlanceLine([
    { name: "Magic Kingdom", wait: 45, ageMs: 0 },
    { name: "EPCOT", wait: 30, ageMs: 60000 },
    { name: "Islands of Adventure", wait: 60, ageMs: 120000 },
  ], NOW);
  assert.strictEqual(line, "MK 45m · EPC 30m · IA 60m");
});

test("buildGlanceLine drops stale entries", function () {
  var line = lib.buildGlanceLine([
    { name: "Magic Kingdom", wait: 45, ageMs: 30 * 60 * 1000 },      // 30m — stale
    { name: "EPCOT", wait: 30, ageMs: 29 * 60 * 1000 },              // just fresh
    { name: "Islands of Adventure", wait: 60, ageMs: 31 * 60 * 1000 },
  ], NOW);
  assert.strictEqual(line, "EPC 30m");
});

test("buildGlanceLine accepts ts in place of ageMs", function () {
  var line = lib.buildGlanceLine([
    { name: "Magic Kingdom", wait: 45, ts: NOW - 60000 },
    { name: "EPCOT", wait: 30, ts: NOW - 31 * 60 * 1000 },  // stale via ts
  ], NOW);
  assert.strictEqual(line, "MK 45m");
});

test("buildGlanceLine drops null/negative/non-numeric waits", function () {
  var line = lib.buildGlanceLine([
    { name: "Magic Kingdom", wait: null, ageMs: 0 },
    { name: "EPCOT", wait: -5, ageMs: 0 },
    { name: "Hollywood Studios", wait: "20", ageMs: 0 },
    { name: "Animal Kingdom", wait: 15, ageMs: 0 },
  ], NOW);
  assert.strictEqual(line, "AK 15m");
});

test("buildGlanceLine empty cases", function () {
  assert.strictEqual(lib.buildGlanceLine([], NOW), "");
  assert.strictEqual(lib.buildGlanceLine([
    { name: "Magic Kingdom", wait: null, ageMs: 0 },
    { name: "", wait: 10, ageMs: 0 },              // no name → no code
    { name: "EPCOT", wait: 10 },                    // no age info
  ], NOW), "");
});

test("buildGlanceLine drops trailing entries over the 60-char budget", function () {
  // Each entry renders as "AAA 100m" (8 chars) joined by " · " (3):
  // 6 entries would be 63 chars, so only the first 5 (52) survive.
  var entries = [];
  for (var i = 0; i < 6; i++) {
    entries.push({ name: "Alpha Aa Aa Park", wait: 100, ageMs: 0 });
  }
  var line = lib.buildGlanceLine(entries, NOW);
  assert.strictEqual(line, "AAA 100m · AAA 100m · AAA 100m · AAA 100m · AAA 100m");
  assert.ok(line.length <= 60, "line over budget: " + line.length);
});

test("buildGlanceLine output never carries wire separators", function () {
  var line = lib.buildGlanceLine([
    { name: "=Evil; |Name", wait: 10, ageMs: 0 },
    { name: "M=K; P|ark", wait: 20, ageMs: 0 },
  ], NOW);
  assert.ok(!/[;=|]/.test(line), "forbidden char in: " + line);
  assert.ok(line.length > 0, "hostile names should still produce codes");
});

// ── buildConfigPage ──────────────────────────────────────────────────────

var CONFIG_DESTS = [
  { id: "dest-dlp", name: "Disneyland Paris", parkCount: 2, lat: 48.9, lon: 2.8 },
  { id: "dest-wdw", name: "Walt Disney World", parkCount: 4, lat: 28.4, lon: -81.6 },
  { id: "dest-adv", name: "Adventureland", parkCount: 1, lat: 41.6, lon: -93.5 },
  { id: "dest-nol", name: "No Location Dest", parkCount: 1, lat: null, lon: null },
];

test("buildConfigPage non-uber: toggle present, no destination section", function () {
  var html = lib.buildConfigPage(lib.defaultSettings(), [], false);
  assert.strictEqual(typeof html, "string");
  assert.ok(html.indexOf('id="glance"') >= 0, "glance checkbox");
  assert.ok(html.indexOf("pebblejs://close#") >= 0, "save URL");
  assert.ok(html.indexOf("Destination") === -1, "no destination section");
  // The page script is shared, so probe for the radio MARKUP, not the
  // bare string "destmode".
  assert.ok(html.indexOf('name="destmode"') === -1, "no destmode radios");
});

test("buildConfigPage uber: filterable grouped destination list", function () {
  var html = lib.buildConfigPage(lib.defaultSettings(), CONFIG_DESTS, true);
  assert.ok(html.indexOf('name="destmode"') >= 0, "destmode radios");
  assert.ok(html.indexOf('value="watch"') >= 0);
  assert.ok(html.indexOf('value="fixed"') >= 0);
  // ~155 destinations: a filter input + bounded scroll list of radios,
  // grouped by region; location-less dest lands in "Other" instead of
  // being dropped. Groups alphabetical.
  assert.ok(html.indexOf('id="destfilter"') >= 0, "filter input");
  assert.ok(html.indexOf('id="destlist"') >= 0, "scroll list");
  assert.ok(html.indexOf("wireFilter('destfilter','destlist')") >= 0, "filter wired");
  var eu = html.indexOf("<h3>Europe</h3>");
  var na = html.indexOf("<h3>North America</h3>");
  var other = html.indexOf("<h3>Other</h3>");
  assert.ok(eu >= 0 && na >= 0 && other >= 0, "expected group headers");
  assert.ok(eu < na && na < other, "groups sorted alphabetically");
  assert.ok(html.indexOf('value="dest-nol"') > other, "location-less in Other");
  // Names sorted within a group: Adventureland before Walt Disney World.
  var adv = html.indexOf('value="dest-adv"');
  var wdw = html.indexOf('value="dest-wdw"');
  assert.ok(na < adv && adv < wdw, "names sorted within group");
  // Rows carry the lowercased search key (name + region).
  assert.ok(html.indexOf('data-s="walt disney world north america"') >= 0,
            "search key on row");
});

test("buildConfigPage prefills from current settings", function () {
  // glance off + fixed WDW: checkbox unchecked, fixed radio checked,
  // WDW radio checked and echoed as the selection, picker enabled.
  var s = { v: 1, glance: false, destMode: "fixed", destId: "dest-wdw" };
  var html = lib.buildConfigPage(s, CONFIG_DESTS, true);
  assert.ok(html.indexOf('id="glance" checked') === -1, "glance unchecked");
  assert.ok(/value="fixed" checked/.test(html), "fixed radio checked");
  assert.ok(!/value="watch" checked/.test(html), "watch radio not checked");
  assert.ok(/value="dest-wdw"[^>]*checked/.test(html), "dest radio checked");
  assert.ok(html.indexOf('id="destselname">Walt Disney World<') >= 0,
            "selection echoed above the list");
  assert.ok(html.indexOf('id="destfilter" placeholder="Filter destinations..." disabled') === -1,
            "filter enabled in fixed mode");

  // Defaults: glance checked, watch mode checked, picker disabled.
  var html2 = lib.buildConfigPage(lib.defaultSettings(), CONFIG_DESTS, true);
  assert.ok(html2.indexOf('id="glance" checked') >= 0, "glance checked");
  assert.ok(/value="watch" checked/.test(html2), "watch radio checked");
  assert.ok(/id="destfilter"[^>]*disabled/.test(html2), "filter starts disabled");
  assert.ok(/name="destpick"[^>]*disabled/.test(html2), "radios start disabled");
  assert.ok(html2.indexOf('id="destselname">none<') >= 0, "no dest preselected");
});

test("buildConfigPage empty dest list disables fixed with hint", function () {
  var s = { v: 1, glance: true, destMode: "fixed", destId: "dest-wdw" };
  var html = lib.buildConfigPage(s, [], true);
  assert.ok(/value="fixed"[^>]*disabled/.test(html), "fixed radio disabled");
  assert.ok(html.indexOf('id="destlist"') === -1, "no dest list rendered");
  assert.ok(html.indexOf("Destination list unavailable") >= 0, "hint shown");
  // Without a list to fix on, the checked mode falls back to watch.
  assert.ok(/value="watch" checked/.test(html), "watch checked as fallback");
});

test("buildConfigPage escapes names", function () {
  var dests = [{ id: 'x"><script>', name: "Bad & <Name>", parkCount: 1,
                 lat: 28.4, lon: -81.6 }];
  var html = lib.buildConfigPage(lib.defaultSettings(), dests, true);
  assert.ok(html.indexOf("Bad &amp; &lt;Name&gt;") >= 0, "text escaped");
  assert.ok(html.indexOf('value="x&quot;&gt;&lt;script&gt;"') >= 0, "attr escaped");
});

test("buildConfigPage is self-contained", function () {
  // The phone webview may be offline: no external fetches allowed.
  var html = lib.buildConfigPage(lib.defaultSettings(), CONFIG_DESTS, true);
  assert.ok(html.indexOf("http://") === -1 && html.indexOf("https://") === -1,
            "no external URLs");
  assert.ok(html.indexOf("pebblejs://close") >= 0, "cancel URL present");
});

test("buildConfigPage timeline toggle present and OFF by default", function () {
  // Both build flavours carry the Timeline card; the checkbox is
  // UNCHECKED unless settings.pins is true (opt-in requirement).
  var uber = lib.buildConfigPage(lib.defaultSettings(), CONFIG_DESTS, true);
  var perDest = lib.buildConfigPage(lib.defaultSettings(), [], false);
  [uber, perDest].forEach(function (html) {
    assert.ok(html.indexOf('id="pins"') >= 0, "pins checkbox present");
    assert.ok(html.indexOf('id="pins" checked') === -1, "unchecked by default");
    assert.ok(html.indexOf("Timeline") >= 0, "card heading");
    assert.ok(html.indexOf("Rebble timeline service") >= 0,
              "hint mentions the Rebble timeline service");
  });
});

test("buildConfigPage timeline toggle prefills and saves", function () {
  var s = lib.defaultSettings();
  s.pins = true;
  var html = lib.buildConfigPage(s, CONFIG_DESTS, true);
  assert.ok(html.indexOf('id="pins" checked') >= 0, "prefilled checked");
  // The save handler must round-trip the checkbox into s.pins.
  assert.ok(html.indexOf("s.pins=document.getElementById('pins').checked") >= 0,
            "save JS includes s.pins");
});

test("buildConfigPage My Parks has filter, chips line and search keys", function () {
  var html = lib.buildConfigPage(lib.defaultSettings(), CONFIG_DESTS, true);
  assert.ok(html.indexOf('id="mpfilter"') >= 0, "parks filter input");
  assert.ok(html.indexOf('id="mplist"') >= 0, "parks scroll list");
  assert.ok(html.indexOf("wireFilter('mpfilter','mplist')") >= 0, "filter wired");
  assert.ok(html.indexOf('id="mpsel"') >= 0, "selected-chips container");
  assert.ok(html.indexOf('id="mpcount"') >= 0, "selected count");
  assert.ok(html.indexOf("updateMyParksCap") >= 0, "cap/chips JS present");
});

// ── buildConfigPage: My Parks section ────────────────────────────────────

var MYPARKS_DESTS = [
  { id: "dest-uni", name: "Universal Orlando", parkCount: 2, lat: 28.47, lon: -81.47,
    parks: [
      { id: "park-ioa", name: "Islands of Adventure", icon: "circle", color: "blue" },
      { id: "park-usf", name: "Universal Studios Florida", icon: "circle", color: "red" },
    ] },
  { id: "dest-wdw", name: "Walt Disney World", parkCount: 2, lat: 28.4, lon: -81.6,
    parks: [
      { id: "park-mk", name: "Magic Kingdom", icon: "castle", color: "pink" },
      { id: "park-ep", name: "EPCOT", icon: "sphere", color: "cyan" },
    ] },
  { id: "dest-empty", name: "Empty Dest", parkCount: 0, lat: 1, lon: 1, parks: [] },
];

var OWN_PARKS = [
  { id: "park-mk", name: "Magic Kingdom" },
  { id: "park-ep", name: "EPCOT" },
];

test("buildConfigPage uber: My Parks grouped by destination", function () {
  var html = lib.buildConfigPage(lib.defaultSettings(), MYPARKS_DESTS, true);
  assert.ok(html.indexOf("My Parks") >= 0, "section present");
  // Group headers alphabetical by destination name; park-less
  // destinations get no header.
  var uni = html.indexOf("<h3>Universal Orlando</h3>");
  var wdw = html.indexOf("<h3>Walt Disney World</h3>");
  assert.ok(uni >= 0 && wdw >= 0, "destination group headers");
  assert.ok(uni < wdw, "groups sorted by destination name");
  assert.ok(html.indexOf("<h3>Empty Dest</h3>") === -1, "empty dest skipped");
  // One checkbox per park, under its destination's header.
  var ioa = html.indexOf('value="park-ioa"');
  var mk = html.indexOf('value="park-mk"');
  assert.ok(ioa > uni && ioa < wdw, "IoA under Universal");
  assert.ok(mk > wdw, "MK under WDW");
  assert.ok(/name="mypark" value="park-ep"/.test(html), "checkbox markup");
});

test("buildConfigPage per-dest: My Parks from the parks argument", function () {
  var html = lib.buildConfigPage(lib.defaultSettings(), [], false, OWN_PARKS);
  assert.ok(html.indexOf("My Parks") >= 0, "section present");
  assert.ok(html.indexOf('value="park-mk"') >= 0, "MK checkbox");
  assert.ok(html.indexOf('value="park-ep"') >= 0, "EPCOT checkbox");
  assert.ok(html.indexOf("<h3>") === -1, "no destination grouping");
  assert.ok(html.indexOf("Park list unavailable") === -1, "no hint");
  // Still no Destination section in per-dest builds.
  assert.ok(html.indexOf('name="destmode"') === -1, "no destmode radios");
});

test("buildConfigPage My Parks prefilled from settings", function () {
  var s = lib.defaultSettings();
  s.myParks = ["park-ep", "park-ioa"];
  var html = lib.buildConfigPage(s, MYPARKS_DESTS, true);
  assert.ok(/value="park-ep"[^>]*checked/.test(html), "EPCOT prechecked");
  assert.ok(/value="park-ioa"[^>]*checked/.test(html), "IoA prechecked");
  assert.ok(!/value="park-mk"[^>]*checked/.test(html), "MK not checked");
});

test("buildConfigPage My Parks max-4 enforcement JS present", function () {
  var html = lib.buildConfigPage(lib.defaultSettings(), MYPARKS_DESTS, true);
  assert.ok(html.indexOf("updateMyParksCap") >= 0, "cap function present");
  assert.ok(html.indexOf("disabled=(n>=4)") >= 0, "disable-at-4 logic");
  // Save handler collects the checkboxes into myParks (clamped again).
  assert.ok(html.indexOf("s.myParks=[]") >= 0, "save collects myParks");
  assert.ok(html.indexOf("s.myParks.length<4") >= 0, "save re-clamps at 4");
});

test("buildConfigPage My Parks unavailable shows hint", function () {
  // Uber with no dest list AND per-dest with no parks both degrade.
  var uber = lib.buildConfigPage(lib.defaultSettings(), [], true);
  assert.ok(uber.indexOf("Park list unavailable") >= 0, "uber hint");
  var perDest = lib.buildConfigPage(lib.defaultSettings(), [], false, []);
  assert.ok(perDest.indexOf("Park list unavailable") >= 0, "per-dest hint");
  assert.ok(perDest.indexOf('name="mypark"') === -1, "no checkboxes");
});

test("buildConfigPage My Parks escapes park names", function () {
  var dests = [{ id: "d1", name: "Dest & <Co>", parkCount: 1, lat: 28.4, lon: -81.6,
                 parks: [{ id: 'p"><script>', name: "Park & <Name>" }] }];
  var html = lib.buildConfigPage(lib.defaultSettings(), dests, true);
  assert.ok(html.indexOf("Park &amp; &lt;Name&gt;") >= 0, "park name escaped");
  assert.ok(html.indexOf('value="p&quot;&gt;&lt;script&gt;"') >= 0,
            "park id escaped");
  assert.ok(html.indexOf("<h3>Dest &amp; &lt;Co&gt;</h3>") >= 0,
            "group header escaped");
});

// ── parseFavsSync ────────────────────────────────────────────────────────
//
// Round-trips against the exact strings the C encoder emits (asserted
// byte-for-byte in tests/unit/test_favs_sync.c) — the two suites pin
// the wire format from both ends.

var FAVS_UUID = "aaaaaaaa-1111-4abc-8def-000000000001";

test("parseFavsSync round-trips the C encoder's typical payload", function () {
  var p = lib.parseFavsSync(FAVS_UUID + "|a:ride0,ride1|h:show0");
  assert.strictEqual(p.parkId, FAVS_UUID);
  assert.deepStrictEqual(p.attractions, ["ride0", "ride1"]);
  assert.deepStrictEqual(p.shows, ["show0"]);
});

test("parseFavsSync round-trips the C encoder's empty payload", function () {
  var p = lib.parseFavsSync(FAVS_UUID + "|a:|h:");
  assert.strictEqual(p.parkId, FAVS_UUID);
  assert.deepStrictEqual(p.attractions, []);
  assert.deepStrictEqual(p.shows, []);
});

test("parseFavsSync tolerates garbage", function () {
  // No usable park id → null, never a throw.
  assert.strictEqual(lib.parseFavsSync(""), null);
  assert.strictEqual(lib.parseFavsSync(null), null);
  assert.strictEqual(lib.parseFavsSync(undefined), null);
  assert.strictEqual(lib.parseFavsSync(42), null);
  assert.strictEqual(lib.parseFavsSync("|a:x|h:y"), null);
  // Park id alone parses to empty lists.
  var bare = lib.parseFavsSync("some-park");
  assert.strictEqual(bare.parkId, "some-park");
  assert.deepStrictEqual(bare.attractions, []);
  assert.deepStrictEqual(bare.shows, []);
  // Unknown kinds and malformed segments are skipped, known ones kept.
  var mixed = lib.parseFavsSync("p1|z:junk|a:r1|:bad|nocolon|h:s1,,s2");
  assert.deepStrictEqual(mixed.attractions, ["r1"]);
  // Empty ids from ",," are dropped.
  assert.deepStrictEqual(mixed.shows, ["s1", "s2"]);
});

// ── buildFavsStore ───────────────────────────────────────────────────────

test("buildFavsStore creates and stamps a new park entry", function () {
  var store = lib.buildFavsStore(null,
    { parkId: "p1", attractions: ["a1"], shows: ["s1"] }, 1234);
  assert.deepStrictEqual(store, { p1: { a: ["a1"], h: ["s1"], ts: 1234 } });
});

test("buildFavsStore replaces the synced park, keeps others", function () {
  var prev = {
    p1: { a: ["old"], h: [], ts: 1 },
    p2: { a: ["keep"], h: ["keep2"], ts: 2 },
  };
  var store = lib.buildFavsStore(prev,
    { parkId: "p1", attractions: [], shows: ["s9"] }, 99);
  // p1 replaced wholesale (full-state sync), p2 carried over untouched.
  assert.deepStrictEqual(store.p1, { a: [], h: ["s9"], ts: 99 });
  assert.deepStrictEqual(store.p2, prev.p2);
  // Pure: prev not mutated.
  assert.deepStrictEqual(prev.p1, { a: ["old"], h: [], ts: 1 });
});

test("buildFavsStore tolerates garbage prev and parsed", function () {
  assert.deepStrictEqual(lib.buildFavsStore("junk", null, 5), {});
  assert.deepStrictEqual(lib.buildFavsStore(undefined, null, 5), {});
  var kept = lib.buildFavsStore({ p1: { a: [], h: [], ts: 1 } }, null, 5);
  assert.deepStrictEqual(kept, { p1: { a: [], h: [], ts: 1 } });
  // Missing lists on parsed degrade to empty arrays.
  var s = lib.buildFavsStore(null, { parkId: "p3" }, 7);
  assert.deepStrictEqual(s.p3, { a: [], h: [], ts: 7 });
});

// ── Timeline pins ────────────────────────────────────────────────────────
//
// Pin times MUST be UTC ISO: the park API's strings carry the park's
// offset, and reusing the zone-less wire "HH:MM" would pin Tokyo's
// parade to the user's local clock. Both hemispheres of the conversion
// are pinned here (Orlando -04:00, Tokyo +09:00).

var MK_UUID = "75ea578a-adc8-4116-a54d-dccb60765ef9";   // shortId 60765ef9

test("isoToUtcIso converts offsets to UTC", function () {
  // Orlando summer: UTC-4.
  assert.strictEqual(lib.isoToUtcIso("2026-08-01T09:00:00-04:00"),
                     "2026-08-01T13:00:00Z");
  // Tokyo: UTC+9 — crosses the date line backwards.
  assert.strictEqual(lib.isoToUtcIso("2026-08-02T08:30:00+09:00"),
                     "2026-08-01T23:30:00Z");
  // Already-UTC passes through, milliseconds stripped.
  assert.strictEqual(lib.isoToUtcIso("2026-08-01T13:00:00Z"),
                     "2026-08-01T13:00:00Z");
  assert.strictEqual(lib.isoToUtcIso("garbage"), null);
  assert.strictEqual(lib.isoToUtcIso(""), null);
  assert.strictEqual(lib.isoToUtcIso(null), null);
});

test("buildHoursPin shape (Orlando -04:00)", function () {
  var pin = lib.buildHoursPin(MK_UUID, "Magic Kingdom",
                              "2026-08-01T09:00:00-04:00",
                              "2026-08-01T22:00:00-04:00");
  assert.deepStrictEqual(pin, {
    id: "tpw-hours-60765ef9-2026-08-01",
    time: "2026-08-01T13:00:00Z",          // UTC, not the -04:00 wall clock
    duration: 780,                          // 13 hours
    layout: {
      // genericPin + subtitle range: the emulator-render-tested layout
      // (calendarPin hides the range; title-range truncates).
      type: "genericPin",
      title: "Magic Kingdom",
      subtitle: "09:00 - 22:00",            // park-local wall clock
      body: "Park hours",
      tinyIcon: "system://images/TIMELINE_CALENDAR",
    },
    actions: [{ title: "Open App", type: "openWatchApp", launchCode: 0 }],
  });
  assert.ok(pin.id.length <= 64, "id within the API's 64-char cap");
});

test("buildHoursPin Tokyo +09:00 keeps the park-local id date", function () {
  var pin = lib.buildHoursPin("aaaaaaaa-bbbb-cccc-dddd-eeee12345678",
                              "Tokyo DisneySea",
                              "2026-08-02T08:30:00+09:00",
                              "2026-08-02T21:00:00+09:00");
  // UTC time rolls back to Aug 1…
  assert.strictEqual(pin.time, "2026-08-01T23:30:00Z");
  // …but the id keeps the PARK'S Aug 2, so the same session is always
  // the same pin whatever the phone's zone.
  assert.strictEqual(pin.id, "tpw-hours-12345678-2026-08-02");
  assert.strictEqual(pin.duration, 750);
});

test("buildHoursPin duration caps at 24h and never goes negative", function () {
  // 25-hour "session" (bad data) caps at 1440.
  var long = lib.buildHoursPin(MK_UUID, "MK",
                               "2026-08-01T09:00:00-04:00",
                               "2026-08-02T10:00:00-04:00");
  assert.strictEqual(long.duration, 1440);
  // close before open → no duration property at all.
  var neg = lib.buildHoursPin(MK_UUID, "MK",
                              "2026-08-01T09:00:00-04:00",
                              "2026-08-01T08:00:00-04:00");
  assert.strictEqual(neg.duration, undefined);
  // Unparseable close → pin still valid, no duration.
  var noClose = lib.buildHoursPin(MK_UUID, "MK",
                                  "2026-08-01T09:00:00-04:00", "junk");
  assert.strictEqual(noClose.duration, undefined);
  assert.strictEqual(noClose.time, "2026-08-01T13:00:00Z");
});

test("buildHoursPin is idempotent and null on garbage", function () {
  var a = lib.buildHoursPin(MK_UUID, "MK", "2026-08-01T09:00:00-04:00",
                            "2026-08-01T22:00:00-04:00");
  var b = lib.buildHoursPin(MK_UUID, "MK", "2026-08-01T09:00:00-04:00",
                            "2026-08-01T22:00:00-04:00");
  assert.deepStrictEqual(a, b);   // same inputs → byte-identical pin
  assert.strictEqual(lib.buildHoursPin(MK_UUID, "MK", "junk", "junk"), null);
  assert.strictEqual(lib.buildHoursPin(MK_UUID, "MK", null, null), null);
  assert.strictEqual(
    lib.buildHoursPin(null, "MK", "2026-08-01T09:00:00-04:00",
                      "2026-08-01T22:00:00-04:00"), null);
});

test("buildShowPin shape and UTC conversion", function () {
  var showId = "aaaaaaaa-bbbb-cccc-dddd-eeeeffff0004";
  var start = "2026-08-01T21:30:00-04:00";
  var pin = lib.buildShowPin(showId, "Fantasmic!", "Hollywood Studios", start);
  assert.strictEqual(pin.time, "2026-08-02T01:30:00Z");
  assert.strictEqual(pin.id,
    "tpw-show-ffff0004-" + Math.floor(Date.parse(start) / 60000));
  assert.ok(/^tpw-show-ffff0004-\d+$/.test(pin.id), "id shape");
  assert.ok(pin.id.length <= 64, "id within the API's 64-char cap");
  assert.deepStrictEqual(pin.layout, {
    type: "genericPin",
    title: "Fantasmic!",
    body: "Hollywood Studios",
    tinyIcon: "system://images/SCHEDULED_EVENT",
  });
  assert.deepStrictEqual(pin.actions,
    [{ title: "Open App", type: "openWatchApp", launchCode: 0 }]);
  // Same showtime → same id (idempotent); different showtime → new id.
  assert.deepStrictEqual(
    lib.buildShowPin(showId, "Fantasmic!", "Hollywood Studios", start), pin);
  var later = lib.buildShowPin(showId, "Fantasmic!", "Hollywood Studios",
                               "2026-08-01T22:30:00-04:00");
  assert.notStrictEqual(later.id, pin.id);
});

test("buildShowPin null on garbage", function () {
  assert.strictEqual(lib.buildShowPin("id", "X", "P", "junk"), null);
  assert.strictEqual(lib.buildShowPin("id", "X", "P", null), null);
  assert.strictEqual(
    lib.buildShowPin(null, "X", "P", "2026-08-01T21:30:00-04:00"), null);
});

// Raw API-shaped live data for selectShowPins: full ids, ISO showtimes.
var PIN_LIVE = [
  { entityType: "SHOW", id: "aaaaaaaa-bbbb-cccc-dddd-eeeeffff1111",
    name: "Parade", status: "OPERATING",
    showtimes: [
      { startTime: "2026-08-01T15:00:00-04:00" },   // past
      { startTime: "2026-08-01T20:00:00-04:00" },
      { startTime: "2026-08-01T23:00:00-04:00" },
    ] },
  { entityType: "SHOW", id: "aaaaaaaa-bbbb-cccc-dddd-eeeeffff2222",
    name: "Fireworks", status: "OPERATING",
    showtimes: [
      { startTime: "2026-08-01T21:00:00-04:00" },
      { startTime: "2026-08-01T22:00:00-04:00" },
    ] },
  { entityType: "SHOW", id: "aaaaaaaa-bbbb-cccc-dddd-eeeeffff3333",
    name: "Unloved Show", status: "OPERATING",
    showtimes: [{ startTime: "2026-08-01T20:30:00-04:00" }] },  // not favourited
  { entityType: "ATTRACTION", id: "aaaaaaaa-bbbb-cccc-dddd-eeeeffff1111",
    name: "Same-shortid Ride", status: "OPERATING" },           // not a SHOW
];
var PIN_FAVS = ["ffff1111", "ffff2222"];
var PIN_NOW = new Date("2026-08-01T19:00:00-04:00").getTime();

test("selectShowPins picks next future favourited showtimes in order", function () {
  var got = lib.selectShowPins(PIN_LIVE, PIN_FAVS, PIN_NOW, 3);
  // 20:00 Parade, 21:00 Fireworks, 22:00 Fireworks — interleaved across
  // shows, sorted by start; the 15:00 (past), the unfavourited show and
  // the 23:00 (over cap) all excluded.
  assert.strictEqual(got.length, 3);
  assert.strictEqual(got[0].name, "Parade");
  assert.strictEqual(got[0].startTime, "2026-08-01T20:00:00-04:00");
  assert.strictEqual(got[1].name, "Fireworks");
  assert.strictEqual(got[1].startTime, "2026-08-01T21:00:00-04:00");
  assert.strictEqual(got[2].name, "Fireworks");
  assert.strictEqual(got[2].startTime, "2026-08-01T22:00:00-04:00");
  // Full ids survive (pin ids need them).
  assert.strictEqual(got[0].id, "aaaaaaaa-bbbb-cccc-dddd-eeeeffff1111");
  // startMs is the parsed instant, usable for expiry bookkeeping.
  assert.strictEqual(got[0].startMs, Date.parse(got[0].startTime));
});

test("selectShowPins cap, defaults and degenerate inputs", function () {
  // max=2 trims; the default cap is SHOW_PINS_MAX (3).
  assert.strictEqual(lib.selectShowPins(PIN_LIVE, PIN_FAVS, PIN_NOW, 2).length, 2);
  assert.strictEqual(lib.SHOW_PINS_MAX, 3);
  assert.strictEqual(lib.selectShowPins(PIN_LIVE, PIN_FAVS, PIN_NOW).length, 3);
  // No favourites, empty/absent live data → empty, never a throw.
  assert.deepStrictEqual(lib.selectShowPins(PIN_LIVE, [], PIN_NOW, 3), []);
  assert.deepStrictEqual(lib.selectShowPins([], PIN_FAVS, PIN_NOW, 3), []);
  assert.deepStrictEqual(lib.selectShowPins(null, null, PIN_NOW, 3), []);
  // Everything in the past → empty.
  var lateNow = new Date("2026-08-02T12:00:00Z").getTime();
  assert.deepStrictEqual(lib.selectShowPins(PIN_LIVE, PIN_FAVS, lateNow, 3), []);
});

test("pinDiff splits prev ids into delete and keep", function () {
  var d = lib.pinDiff(["a", "b", "c"], ["b", "c", "d"]);
  assert.deepStrictEqual(d, { toDelete: ["a"], toKeep: ["b", "c"] });
  // New ids ("d") are the pusher's business, not the differ's.
  assert.deepStrictEqual(lib.pinDiff([], ["x"]), { toDelete: [], toKeep: [] });
  assert.deepStrictEqual(lib.pinDiff(["x"], []), { toDelete: ["x"], toKeep: [] });
  assert.deepStrictEqual(lib.pinDiff([], []), { toDelete: [], toKeep: [] });
  assert.deepStrictEqual(lib.pinDiff(null, null), { toDelete: [], toKeep: [] });
});

// ── summary ──────────────────────────────────────────────────────────────

console.log(passed + " passed, " + failed + " failed");
process.exitCode = failed ? 1 : 0;
