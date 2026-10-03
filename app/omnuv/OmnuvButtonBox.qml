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
    // **One order on every platform** (the parity sheet, 3 October 2026): the
    // style's own layout put Delete before Cancel on Windows and after it on
    // Linux. Mac's order, Cancel then the action, is the web's: the primary
    // is the rightmost button, so the eye and the pointer find it in one place.
    buttonLayout: DialogButtonBox.MacLayout
    background: Rectangle {
        // The web's sunken surface: the style's own base was white on a dark
        // dialog, under white button text (3 October 2026).
        color: Theme.highContrast ? palette.base : Theme.fillSubtle
        bottomLeftRadius: Theme.radiusOverlay
        bottomRightRadius: Theme.radiusOverlay
    }
}
