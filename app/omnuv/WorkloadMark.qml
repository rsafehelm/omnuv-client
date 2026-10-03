import QtQuick

import Omnuv 1.0

// **What the machine is for, as a mark on its card** (the operator, 3 October
// 2026: "a stylized logo or representation representing the workload, for all
// workloads"). A white tile on the card's picture, with:
//
//   chat      a speech bubble in the accent's gradient (Ollama + Open WebUI)
//   web       a globe, for any other web recipe
//   game      a game pad (a streamed machine)
//   windows   the four panes, in Microsoft's blue
//   gpu       NVIDIA's eye (a GPU machine with nothing more specific)
//   linux     Tux
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

    // Four panes, the second-generation shape, in Microsoft's blue.
    Grid {
        visible: mark.workload === "windows"
        anchors.centerIn: tile
        columns: 2
        spacing: 2
        Repeater {
            model: 4
            Rectangle { width: 11; height: 11; color: "#0078D4" }
        }
    }

    Glyph {
        visible: Theme.iconsInstalled && (mark.workload === "game" || mark.workload === "web")
        anchors.centerIn: tile
        icon: mark.workload === "game" ? Theme.icon.game : Theme.icon.globe
        size: 22
        color: Theme.accent
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
