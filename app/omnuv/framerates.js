// Omnuv: the frame rates Stream settings offers.
//
// **The usual rates, 30 to 480, whatever this display is** (the operator,
// 5 October 2026: a 360 Hz laptop was offered 60 72 90 120 180 360 and not
// 144, 165 or 240, and asked for 480 too). They are the rates the Windows
// image's virtual display offers (onv_windows_vdd.rates); with V-Sync off,
// the default since 2 October, an uneven rate drops nothing, and a rate above
// the screen's still shortens the wait for a frame. Each display's own rate is
// added beside them. With V-Sync on, the rates even on this display come
// first: that is the one case an uneven rate costs frames.
//
// A library, and pure, so `test/framerates_test.js` runs it under node.
.pragma library

var USUAL = [30, 60, 90, 120, 144, 165, 240, 360, 480]

function evenOn(displayRate, rate) {
    return displayRate > 0 && rate > 0 && displayRate % rate === 0
}

// `displayRates`: each display's refresh rate, as SystemProperties reads them.
// `displayRate`: the primary display's. `vsync`: StreamingPreferences.enableVsync.
function offered(displayRates, displayRate, vsync) {
    var seen = {}
    var rates = USUAL.concat(displayRates || []).filter(function (r) {
        return r > 0 && (seen[r] ? false : (seen[r] = true))
    })
    rates.sort(function (a, b) {
        if (vsync) {
            var ea = evenOn(displayRate, a) ? 0 : 1, eb = evenOn(displayRate, b) ? 0 : 1
            if (ea !== eb) return ea - eb
        }
        return a - b
    })
    return rates
}
