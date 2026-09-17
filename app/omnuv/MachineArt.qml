// Omnuv: the picture at the top of a machine card.
//
// Windows App leads each Cloud PC with its wallpaper and washes the rest of
// the card in that wallpaper's colour, so two machines are told apart before a
// word is read. A rented machine has no wallpaper, so this paints one: folds of
// light in the person's own accent colour, turned a little round the wheel by
// the machine's name. Same name, same picture, on every device and every day —
// it is an identity, not a decoration that shuffles.
//
// A machine that is not running keeps its picture and loses its colour, the
// way a screen that is off keeps its shape.
//
// Painted once per size, name, state and theme, into an image: nothing here
// animates and nothing here needs a GPU, so it is the same picture under the
// software backend and under Direct3D.

import QtQuick

import Omnuv 1.0

Canvas {
    id: art

    property string seed: ""
    property bool asleep: false
    // The card's corner, so the picture follows it: clipping in Qt Quick is
    // rectangular, and a square picture in a rounded card shows its corners.
    property real radius: Theme.radiusCard

    // A small stable number from the name. Not cryptography: it only has to
    // spread a handful of machines across a handful of hues.
    readonly property int hash: {
        var h = 7
        for (var i = 0; i < seed.length; i++) {
            h = (h * 31 + seed.charCodeAt(i)) % 9973
        }
        return h
    }
    readonly property real accentHue: Theme.accent.hslHue < 0 ? 0.58 : Theme.accent.hslHue
    // A little either side of the accent — enough to tell two machines apart,
    // not enough to leave the family of colours the person chose.
    readonly property real hue: {
        var h = accentHue + ((hash % 9) - 4) * 0.022
        return h - Math.floor(h)
    }
    // What the rest of the card is washed with.
    readonly property color tint: Qt.hsla(hue, asleep ? 0.05 : 0.85, Theme.onDarkSurface ? 0.60 : 0.50, 1)

    readonly property string key: [width, height, seed, asleep, Theme.onDarkSurface, accentHue].join("|")
    onKeyChanged: requestPaint()

    function css(h, s, l, a) {
        var c = Qt.hsla(h - Math.floor(h), asleep ? s * 0.06 : s, l, 1)
        return "rgba(" + Math.round(c.r * 255) + "," + Math.round(c.g * 255) + "," + Math.round(c.b * 255) + "," + a + ")"
    }

    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()
        var w = width, h = height, r = radius
        if (w < 8 || h < 8) {
            return
        }
        var dark = Theme.onDarkSurface

        // Rounded above, square below: the body of the card continues it.
        ctx.beginPath()
        ctx.moveTo(0, h)
        ctx.lineTo(0, r)
        ctx.arcTo(0, 0, r, 0, r)
        ctx.lineTo(w - r, 0)
        ctx.arcTo(w, 0, w, r, r)
        ctx.lineTo(w, h)
        ctx.closePath()
        ctx.clip()

        // The sky.
        var sky = ctx.createLinearGradient(0, 0, w, h)
        sky.addColorStop(0, css(hue + 0.01, dark ? 0.55 : 0.60, dark ? 0.16 : 0.90, 1))
        sky.addColorStop(1, css(hue + 0.05, dark ? 0.65 : 0.70, dark ? 0.07 : 0.74, 1))
        ctx.fillStyle = sky
        ctx.fillRect(0, 0, w, h)

        // The petals: long leaves fanned from a point below the picture, laid
        // left to right so each one's lit edge falls across the shaded side
        // of the one before — which is all a fold is. Windows' bloom, reduced
        // to what a card's width can carry.
        var petals = 7
        var lean = ((hash % 5) - 2) * 0.04
        var bx = w * (0.64 + lean)
        var by = h * 1.50
        var rx = h * 0.38
        var ry = h * 0.88
        var sat = dark ? 0.88 : 0.72
        for (var k = 0; k < petals; k++) {
            var t = k / (petals - 1)
            var angle = (-70 + 140 * t) * Math.PI / 180
            ctx.save()
            ctx.translate(bx, by)
            ctx.rotate(angle)
            ctx.translate(0, -ry * 0.95)
            var g = ctx.createLinearGradient(-rx, 0, rx, 0)
            g.addColorStop(0.0, css(hue + 0.012 * k, sat, dark ? 0.82 : 0.84, 0.94))
            g.addColorStop(0.5, css(hue + 0.016 * k, sat, dark ? 0.64 : 0.68, 0.92))
            g.addColorStop(1.0, css(hue + 0.024 * k, sat, dark ? 0.44 : 0.50, 0.92))
            ctx.scale(rx / ry, 1)
            ctx.beginPath()
            ctx.arc(0, 0, ry, 0, Math.PI * 2)
            ctx.restore()
            ctx.fillStyle = g
            ctx.fill()
        }

        // A last breath of light from the upper left, over everything.
        var glow = ctx.createRadialGradient(0, 0, 0, 0, 0, w * 0.7)
        glow.addColorStop(0, "rgba(255,255,255," + (dark ? 0.10 : 0.28) + ")")
        glow.addColorStop(1, "rgba(255,255,255,0)")
        ctx.fillStyle = glow
        ctx.fillRect(0, 0, w, h)
    }
}
