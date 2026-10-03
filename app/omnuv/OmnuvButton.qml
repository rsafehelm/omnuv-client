// Omnuv: a button the web's way, drawn by us in every style (3 October 2026).
//
//   highlighted or checked   filled with Theme's accent, white text — the
//                            primary action, a chosen chip
//   flat                     no fill until pointed at — a quiet action
//   otherwise                the web's sunken fill, ink text — a secondary
//
// **Why not the style's.** Material raises a button by a shadow drawn as a
// layer effect, and the software renderer — the Linux loop's, and any
// buyer's machine without working graphics drivers — draws no layer effects
// at all: every raised button's background vanished and its white label sat
// on the white page (the Material photograph of 3 October 2026). A rectangle
// is drawn identically by every backend and every style. Under high contrast
// the palette's own pair is used, as everywhere else.

import QtQuick
import QtQuick.Controls

Button {
    id: control

    readonly property bool filled: highlighted || checked
    readonly property color fill: Theme.highContrast ? (filled ? palette.highlight : palette.button)
        : filled ? Theme.accent
        : flat ? (hovered && enabled ? Theme.fillSubtle : "transparent")
        : Theme.fillSubtle
    readonly property color ink: Theme.highContrast ? (filled ? palette.highlightedText : palette.buttonText)
        : filled ? "white" : Theme.ink

    implicitHeight: 34
    // Material keeps 6 px of empty inset round a button for its shadow,
    // which made these 22 px tall; the web's are drawn edge to edge.
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    leftPadding: 14
    rightPadding: 14
    topPadding: 6
    bottomPadding: 6
    font.family: Theme.textFamily
    font.pixelSize: 14
    font.weight: filled ? Theme.strongWeight : Theme.regularWeight
    hoverEnabled: true

    background: Rectangle {
        radius: Theme.radiusButton
        color: control.fill
        opacity: control.enabled ? 1 : 0.5
        border.width: control.visualFocus ? 2 : (Theme.highContrast || (!control.filled && !control.flat) ? 1 : 0)
        border.color: control.visualFocus ? Theme.accent : Theme.strokeCard
        // Pressed: a shade, drawn without an effect.
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: control.filled ? "black" : Theme.ink
            opacity: control.down ? 0.12 : control.hovered && control.enabled ? 0.05 : 0
        }
    }

    contentItem: Label {
        text: control.text
        font: control.font
        color: control.ink
        opacity: control.enabled ? 1 : 0.7
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
