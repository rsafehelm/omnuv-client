// Omnuv: band one — whose cloud this is, and which project is in view.
//
// It never scrolls away, because it is the answer to "what am I looking at".
// The project switch is a row of segments rather than a menu: a person with
// three projects should see all three names, and the selected one is the only
// thing on the band drawn in the accent. With one project there is nothing to
// switch, so the row is not drawn — a switch with one position is furniture.

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import Omnuv 1.0

ColumnLayout {
    id: band
    spacing: Theme.spacing

    readonly property var estate: Omnuv.estate
    readonly property var members: estate.members.data

    // "Acme Robotics · 3 projects · 2 members · you are the owner". A part
    // whose source has not been read is left out rather than guessed.
    function summary() {
        var parts = []
        if (estate.organizationName !== "") {
            parts.push(estate.organizationName)
        }
        var n = Omnuv.projectIds.length
        if (n > 0) {
            parts.push(n === 1 ? qsTr("1 project") : qsTr("%1 projects").arg(n))
        }
        if (members !== undefined && members.length !== undefined) {
            parts.push(members.length === 1 ? qsTr("1 member") : qsTr("%1 members").arg(members.length))
        }
        if (estate.organizationName !== "") {
            parts.push(estate.owner ? qsTr("you are the owner") : qsTr("you are a member"))
        }
        return parts.join(" · ")
    }

    Label {
        Layout.fillWidth: true
        text: band.summary()
        visible: text !== ""
        elide: Label.ElideRight
        font.family: Theme.textFamily
        font.pixelSize: Theme.bodySize
        opacity: 0.8
    }

    // The ids are the values and the names the labels: two projects may share
    // a name, and choosing by name would choose the wrong network.
    Row {
        id: segments
        visible: Omnuv.projectIds.length > 1
        spacing: Theme.spacingTight

        Repeater {
            model: Omnuv.projectIds

            Button {
                id: segment
                readonly property bool selected: modelData === Omnuv.projectId
                text: Omnuv.projectNames[index]
                flat: !selected
                highlighted: selected
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                font.weight: selected ? Theme.strongWeight : Theme.regularWeight
                Accessible.name: selected ? qsTr("%1, the project in view").arg(text) : text
                onClicked: Omnuv.selectProject(modelData)
            }
        }
    }
}
