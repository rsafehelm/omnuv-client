// The button row of every dialog in the window, opaque (2 October 2026).
//
// The style's own DialogButtonBox draws its band with a translucent layer
// fill, and with the window's background given to Mica the page behind showed
// through it (the operator: "the popup must be opaque"). The dialogs' bodies
// were made opaque on 25 September; this is the same fix for the row under
// them, kept in one place so no dialog can be left out. A shade of the
// dialog's own base, solid, so the row still reads as a footer.
import QtQuick
import QtQuick.Controls

DialogButtonBox {
    background: Rectangle {
        color: Qt.tint(palette.base, Theme.highContrast ? "transparent" : "#0B000000")
        bottomLeftRadius: Theme.radiusOverlay
        bottomRightRadius: Theme.radiusOverlay
    }
}
