import QtQuick

import Omnuv 1.0

// **What the machine is for, as a mark on its card** (the operator, 3 October
// 2026: "a stylized logo or representation representing the workload, for all
// workloads"). A white tile on the card's picture, with:
//
//   chat      a speech bubble in the accent's gradient (Ollama + Open WebUI)
//   web       a globe, for any other web recipe
//   game      a game pad (a streamed machine)
//   windows   the word Windows over a generic window
//   gpu       NVIDIA's eye (a GPU machine with nothing more specific)
//   linux     Tux
//   machine   a plain display, when nothing says what it is
//
// Which one is MachineModel::markFor's: Core's mark and the image's OS,
// Windows first (the operator, 4 October 2026). Not Microsoft's logo: its
// trademark guidelines allow none of its logos without a licence and allow
// the word (omnuv src/console-shared/src/logos/ATTRIBUTION.md).
//
// and NVIDIA's eye as a small badge on the tile's corner when the machine has
// a card and the main mark is not already the eye. The two logos are files in
// logos/ that name their source, licence and trademark holder; the rest is
// drawn here, in the card's own colours.
Item {
    id: mark

    property string workload: "linux"
    property bool hasGpu: false
    property bool asleep: false

    width: 44
    height: 44

    Rectangle {
        id: tile
        anchors.fill: parent
        radius: Theme.radiusOverlay + 2
        color: Theme.onDarkSurface ? "#E6FFFFFF" : "#F5FFFFFF"
        border.width: 1
        border.color: "#14000000"
        opacity: mark.asleep ? 0.75 : 1
    }

    // ---- The marks -------------------------------------------------------
    Image {
        visible: mark.workload === "linux"
        anchors.centerIn: tile
        width: 26
        height: 30
        source: "logos/tux.svg"
        sourceSize: Qt.size(width * 2, height * 2)
        fillMode: Image.PreserveAspectFit
        smooth: true
    }

    Image {
        visible: mark.workload === "gpu"
        anchors.centerIn: tile
        width: 30
        height: 20
        source: "logos/nvidia.svg"
        sourceSize: Qt.size(width * 2, height * 2)
        fillMode: Image.PreserveAspectFit
        smooth: true
    }

    // The word over a generic window: the word is what says Windows, so it
    // is on top, clear of the card's badge in the bottom corner. The tile is
    // always near-white, so the ink is a fixed dark one.
    Column {
        visible: mark.workload === "windows"
        anchors.centerIn: tile
        spacing: 2
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Windows"
            color: "#1F2937"
            font.pixelSize: 8
            font.weight: Font.DemiBold
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 22
            height: 17
            radius: 2.5
            color: "transparent"
            border.width: 2
            border.color: "#1F2937"
            Rectangle {
                width: parent.width
                height: 5
                radius: 2.5
                color: "#1F2937"
            }
        }
    }

    // A plain display: an OS nobody names, and no purpose either.
    Item {
        visible: mark.workload === "machine"
        anchors.centerIn: tile
        width: 28
        height: 24
        Rectangle {
            width: 28
            height: 18
            radius: 2.5
            color: "transparent"
            border.width: 2
            border.color: "#6B7280"
        }
        Rectangle { x: 13; y: 18; width: 2; height: 4; color: "#6B7280" }
        Rectangle { x: 8; y: 22; width: 12; height: 2; radius: 1; color: "#6B7280" }
    }

    // The game pad and the globe are drawn, not taken from the icon font:
    // that font is Windows' own, and on macOS and Linux the tile was empty.
    Canvas {
        id: drawn
        visible: mark.workload === "game" || mark.workload === "web"
        anchors.centerIn: tile
        width: 32
        height: 26
        readonly property color ink: Theme.accent
        readonly property string what: mark.workload
        onInkChanged: requestPaint()
        onWhatChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var w = width, h = height
            if (what === "game") {
                // A body with two grips, a cross on the left, two buttons
                // on the right: the shape every platform uses for "play".
                var g = ctx.createLinearGradient(0, 0, w, h)
                g.addColorStop(0, ink)
                g.addColorStop(1, Qt.darker(ink, 1.35))
                ctx.fillStyle = g
                ctx.beginPath()
                ctx.moveTo(8, 4)
                ctx.lineTo(w - 8, 4)
                ctx.bezierCurveTo(w - 1, 4, w + 1, h - 1, w - 4, h - 1)
                ctx.bezierCurveTo(w - 8, h - 1, w - 9, h - 7, w - 12, h - 7)
                ctx.lineTo(12, h - 7)
                ctx.bezierCurveTo(9, h - 7, 8, h - 1, 4, h - 1)
                ctx.bezierCurveTo(-1, h - 1, 1, 4, 8, 4)
                ctx.closePath()
                ctx.fill()
                ctx.fillStyle = "white"
                ctx.fillRect(7, 10, 8, 2.6)
                ctx.fillRect(9.7, 7.3, 2.6, 8)
                ctx.beginPath(); ctx.arc(w - 10, 9.5, 1.9, 0, Math.PI * 2); ctx.fill()
                ctx.beginPath(); ctx.arc(w - 6.5, 13, 1.9, 0, Math.PI * 2); ctx.fill()
            } else {
                // A globe: the outline, the equator, two meridians.
                var r = Math.min(w, h) / 2 - 1.5
                var cx = w / 2, cy = h / 2
                ctx.strokeStyle = ink
                ctx.lineWidth = 2
                ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(cx - r, cy); ctx.lineTo(cx + r, cy); ctx.stroke()
                ctx.save(); ctx.translate(cx, cy); ctx.scale(0.45, 1)
                ctx.beginPath(); ctx.arc(0, 0, r, 0, Math.PI * 2); ctx.restore(); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(cx - r * 0.85, cy - r * 0.5); ctx.lineTo(cx + r * 0.85, cy - r * 0.5); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(cx - r * 0.85, cy + r * 0.5); ctx.lineTo(cx + r * 0.85, cy + r * 0.5); ctx.stroke()
            }
        }
    }

    // The speech bubble: a rounded body and a tail, in the accent's
    // gradient, with three dots for a conversation under way.
    Canvas {
        id: bubble
        visible: mark.workload === "chat"
        anchors.centerIn: tile
        width: 30
        height: 28
        readonly property color from: Theme.accent
        readonly property color to: Qt.hsla((Theme.accent.hslHue < 0 ? 0.75 : Theme.accent.hslHue) + 0.08 - Math.floor(Theme.accent.hslHue + 0.08),
                                            0.75, 0.55, 1)
        onFromChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var w = width, h = height, r = 8
            var bh = h - 6
            var g = ctx.createLinearGradient(0, 0, w, bh)
            g.addColorStop(0, from)
            g.addColorStop(1, to)
            ctx.fillStyle = g
            ctx.beginPath()
            ctx.moveTo(r, 0)
            ctx.lineTo(w - r, 0)
            ctx.arcTo(w, 0, w, r, r)
            ctx.lineTo(w, bh - r)
            ctx.arcTo(w, bh, w - r, bh, r)
            ctx.lineTo(12, bh)
            ctx.lineTo(5, h)
            ctx.lineTo(7, bh)
            ctx.lineTo(r, bh)
            ctx.arcTo(0, bh, 0, bh - r, r)
            ctx.lineTo(0, r)
            ctx.arcTo(0, 0, r, 0, r)
            ctx.closePath()
            ctx.fill()
            ctx.fillStyle = "white"
            for (var i = 0; i < 3; i++) {
                ctx.beginPath()
                ctx.arc(w / 2 + (i - 1) * 7, bh / 2, 2.2, 0, Math.PI * 2)
                ctx.fill()
            }
        }
    }

    // ---- The card underneath ---------------------------------------------
    Rectangle {
        visible: mark.hasGpu && mark.workload !== "gpu"
        width: 20
        height: 20
        radius: 10
        x: tile.width - width + 6
        y: tile.height - height + 6
        color: "white"
        border.width: 1
        border.color: "#1F000000"

        Image {
            anchors.centerIn: parent
            width: 14
            height: 10
            source: "logos/nvidia.svg"
            sourceSize: Qt.size(width * 3, height * 3)
            fillMode: Image.PreserveAspectFit
            smooth: true
        }
    }
}
