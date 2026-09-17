// Omnuv: a short label on a faint wash of its tone — a machine's state, a
// spec, a count. The word always carries the meaning; the tone only helps.
// Used by the cards, the pulse row and the messages panel so the three agree.

import QtQuick 2.9
import QtQuick.Controls 2.2

import Omnuv 1.0

Rectangle {
    id: pill

    property alias text: label.text
    property color tone: Theme.fillNeutral
    // A dot before the word, for a state rather than a fact.
    property bool dot: false
    property bool strong: false

    implicitHeight: 22
    implicitWidth: row.implicitWidth + 2 * Theme.spacing + 2
    radius: height / 2
    color: Qt.rgba(tone.r, tone.g, tone.b, 0.16)

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spacingTight + 2

        Rectangle {
            visible: pill.dot
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 3
            color: pill.tone
        }

        Label {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.textFamily
            font.pixelSize: Theme.captionSize
            font.weight: pill.strong ? Theme.strongWeight : Theme.regularWeight
        }
    }
}
