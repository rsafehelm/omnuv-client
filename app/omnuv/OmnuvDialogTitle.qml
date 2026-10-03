import QtQuick
import QtQuick.Controls

import Omnuv 1.0

// **A dialog's title on the dialog's own surface** (3 October 2026). Material
// draws a dialog's header on its own `dialogColor`, a grey band over our
// card-coloured surface; the web's dialogs have a plain title. Used as
// `header: OmnuvDialogTitle { text: <dialog>.title }`.
Label {
    padding: 24
    bottomPadding: 0
    wrapMode: Text.WordWrap
    font.family: Theme.displayFamily
    font.pixelSize: Theme.subtitleSize
    font.weight: Theme.strongWeight
    color: palette.windowText
    background: null
    Accessible.role: Accessible.Heading
}
