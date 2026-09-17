// Omnuv: the estate's words — what the window says about a project, built
// from Core's fields and nothing else.
//
// **Ported from the web console, not reinvented.** `HistoryBand.svelte`,
// `format.ts` and the estate page say these sentences first; a change that
// reads one way in the browser and another in the tray is the product telling
// a person two things about one event. When the console's wording moves, this
// file moves with it.
//
// A library, so every file that imports it shares one copy, and pure, so
// `test/estate_test.js` can run it under node without a Qt.
.pragma library

// A buyer's vocabulary for each table the map records. The subject carries its
// own article so "an SSH key" needs no vowel test.
//
// `instances.create` is *requested*, because the epoch is minted when the
// marketplace agreed, before anything booted. Every `update` is *changed*:
// the history endpoint does not return before and after, and naming what
// changed from the intent alone would be asserting an observation nobody made.
var WORDS = {
    instances:          { subject: "a machine",         create: "requested", update: "changed", delete: "deleted" },
    networks:           { subject: "a private network", create: "created",   update: "changed", delete: "removed" },
    network_addresses:  { subject: "an address",        create: "assigned",  update: "changed", delete: "released" },
    endpoints:          { subject: "a public endpoint", create: "published", update: "changed", delete: "withdrawn" },
    recipe_deployments: { subject: "a deployment",      create: "deployed",  update: "changed", delete: "removed" },
    ssh_keys:           { subject: "an SSH key",        create: "added",     update: "changed", delete: "removed" },
    api_keys:           { subject: "an API key",        create: "created",   update: "changed", delete: "revoked" },
    devices:            { subject: "a device",          create: "enrolled",  update: "changed", delete: "removed" }
}

var UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i

// The name in front of the sentence, or nothing. A uuid is never printed: it
// is not a name and a buyer cannot act on it. `?` is the map's sentinel for a
// row with neither an id nor an address, and is as unprintable.
function nameOf(c) {
    var named = String(c.resource_name || "").trim()
    if (named) {
        return named
    }
    // An address's id is the address itself, which a buyer does read.
    var v = String(c.resource_id || "").trim()
    if (!v || v === "?" || UUID.test(v)) {
        return ""
    }
    return v
}

// Core's reasons describe the change in the third person ("the buyer created
// an api key"). The actor is already on the row, so it comes off; what is left
// is dropped when it only repeats the verb. A reason that adds something —
// "no free RTX 3090, nothing is charged" — survives.
function reasonClause(reason, verb) {
    var rest = String(reason || "").replace(/^(the buyer|the marketplace|omnuv|a member)\s+/i, "").trim()
    if (!rest || rest.toLowerCase().indexOf(verb.toLowerCase()) === 0) {
        return ""
    }
    return rest
}

function sentence(c) {
    // An unmapped table is still shown, in the least wrong words available:
    // silence about a change is worse than a clumsy sentence about it.
    var w = WORDS[c.resource] || {
        subject: "a " + String(c.resource).replace(/_/g, " ").replace(/s$/, ""),
        create: "created", update: "changed", delete: "removed"
    }
    var verb = w[c.intent] || "changed"
    var name = nameOf(c)
    var head = name ? name + " " + verb : w.subject + " was " + verb
    if (!name) {
        head = head.charAt(0).toUpperCase() + head.slice(1)
    }
    var why = reasonClause(c.reason, verb)
    return why ? head + " — " + why : head
}

// "just now", "4m ago", "in 7d". `now` is milliseconds, passed in so a view
// can tick it and a test can fix it.
function ago(iso, now) {
    if (!iso) {
        return "never"
    }
    var at = new Date(iso).getTime()
    if (isNaN(at)) {
        return ""
    }
    var diff = now - at
    var ahead = diff < 0
    var mins = Math.round(Math.abs(diff) / 60000)
    function wrap(s) { return ahead ? "in " + s : s + " ago" }
    if (mins < 1) {
        return ahead ? "now" : "just now"
    }
    if (mins < 60) {
        return wrap(mins + "m")
    }
    var hours = Math.round(mins / 60)
    if (hours < 24) {
        return wrap(hours + "h")
    }
    return wrap(Math.round(hours / 24) + "d")
}

// Two decimals for anything a person would pay in cash, and enough to be
// non-zero below a cent — a €0.0013 inference is not €0.00.
function money(value, currency) {
    var n = Number(value)
    if (isNaN(n)) {
        return "—"
    }
    var c = currency || "EUR"
    var symbol = c === "EUR" ? "€" : (c === "USD" ? "$" : c + " ")
    if (n !== 0 && Math.abs(n) < 0.01) {
        return symbol + n.toFixed(6).replace(/0+$/, "")
    }
    return symbol + n.toFixed(2)
}

function bytes(n) {
    if (n < 1024) {
        return n + " B"
    }
    var units = ["KB", "MB", "GB", "TB"]
    var v = n / 1024
    var u = 0
    while (v >= 1024 && u < units.length - 1) {
        v /= 1024
        u++
    }
    return v.toFixed(v < 10 ? 1 : 0) + " " + units[u]
}

// Thirty points, one per day, oldest first. Core sends `by_day` sparsely — a
// row only for a day something was spent — so a day it did not mention is a
// day nothing was spent, not a gap to draw across. Days are matched in UTC,
// as Core truncates them.
function series(usage, now) {
    if (!usage || !usage.by_day) {
        return []
    }
    var byDay = {}
    for (var i = 0; i < usage.by_day.length; i++) {
        byDay[usage.by_day[i].day] = Number(usage.by_day[i].cost)
    }
    var out = []
    for (var d = 29; d >= 0; d--) {
        var day = new Date(now - d * 86400000).toISOString().slice(0, 10)
        out.push(byDay[day] || 0)
    }
    return out
}

// The pulse row. A counter whose source could not be read is **left out**,
// never shown as 0: a failed read is not an empty estate.
function pulse(counts, machinesLoaded, parked) {
    var out = []
    if (machinesLoaded) {
        out.push({ label: "running", n: counts["Running"] || 0, tone: "running" })
        out.push({ label: "starting", n: counts["Starting"] || 0, tone: "starting" })
    }
    if (parked) {
        out.push({ label: "waiting", n: waiting(parked).length, tone: "waiting" })
    }
    if (machinesLoaded) {
        out.push({ label: "stopped", n: counts["Stopped"] || 0, tone: "stopped" })
    }
    return out
}

function waiting(parked) {
    var out = []
    for (var i = 0; i < (parked || []).length; i++) {
        if (parked[i].status === "WAITING") {
            out.push(parked[i])
        }
    }
    return out
}

// One banner for the band, not one per read: a stale read is a statement about
// the screen. Returns the reads that could not refresh.
function staleOf(reads) {
    var out = []
    for (var i = 0; i < reads.length; i++) {
        if (reads[i] && reads[i].state === "stale") {
            out.push(reads[i])
        }
    }
    return out
}
