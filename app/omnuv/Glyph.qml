// Omnuv: one Segoe Fluent glyph, by its name in `Theme.icon`.
//
// It draws nothing where the icon font is absent — a Linux desktop, a Mac —
// rather than an empty box, so it is only ever decoration: the word it sits
// beside carries the meaning, which is the rule for colour as well.

import QtQuick
import QtQuick.Controls

import Omnuv 1.0

Label {
    property string icon
    property int size: 16

    visible: Theme.iconsInstalled && icon !== ""
    text: icon
    font.family: Theme.iconFamily
    font.pixelSize: size
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    Accessible.ignored: true
}
