// Omnuv: light behind the top of the window.
//
// Two very soft pools of the person's accent colour, one from each upper
// corner, fading to nothing before the machines begin. It is what gives a flat
// window a top and a depth — the Windows 11 Settings home page and the Store do
// the same with their hero art — and it is faint on purpose: at a glance the
// window is simply not grey.
//
// Painted once per size and theme into an image, like the cards' pictures, so
// it costs nothing at rest and needs no GPU.

import QtQuick

import Omnuv 1.0

Canvas {
    id: aurora

    readonly property real accentHue: Theme.accent.hslHue < 0 ? 0.58 : Theme.accent.hslHue
    readonly property string key: [width, height, Theme.onDarkSurface, accentHue].join("|")
    onKeyChanged: requestPaint()

    function pool(ctx, x, y, radius, hue, alpha) {
        var c = Qt.hsla(hue - Math.floor(hue), 0.90, Theme.onDarkSurface ? 0.55 : 0.60, 1)
        var rgb = Math.round(c.r * 255) + "," + Math.round(c.g * 255) + "," + Math.round(c.b * 255)
        var g = ctx.createRadialGradient(x, y, 0, x, y, radius)
        g.addColorStop(0, "rgba(" + rgb + "," + alpha + ")")
        g.addColorStop(0.5, "rgba(" + rgb + "," + alpha * 0.35 + ")")
        g.addColorStop(1, "rgba(" + rgb + ",0)")
        ctx.fillStyle = g
        ctx.fillRect(0, 0, width, height)
    }

    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()
        if (width < 8 || height < 8) {
            return
        }
        var strength = Theme.onDarkSurface ? 0.22 : 0.20
        pool(ctx, width * 0.08, -height * 0.25, Math.max(width * 0.55, height * 1.4), accentHue, strength)
        pool(ctx, width * 0.92, -height * 0.35, Math.max(width * 0.45, height * 1.3), accentHue + 0.16, strength * 0.8)

        // And out, before the lower edge: the pools are wider than this
        // picture is tall, and a glow that ends in a line is a stripe.
        var fade = ctx.createLinearGradient(0, 0, 0, height)
        fade.addColorStop(0, "rgba(0,0,0,1)")
        fade.addColorStop(0.45, "rgba(0,0,0,0.75)")
        fade.addColorStop(1, "rgba(0,0,0,0)")
        ctx.globalCompositeOperation = "destination-in"
        ctx.fillStyle = fade
        ctx.fillRect(0, 0, width, height)
    }
}
