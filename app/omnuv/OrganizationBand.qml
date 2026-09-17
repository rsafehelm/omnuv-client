// Omnuv: the top bar — whose cloud this is, which project is in view, and the
// window's two actions. What just happened is said in the messages panel at
// the bottom, not here. One row, no frame: it is the answer to "what am I
// looking at", so it never scrolls away and never competes with the machines.
//
// The project switch is a row of segments rather than a menu: a person with
// three projects should see all three names, and the selected one is the only
// thing on the bar drawn in the accent. With one project there is nothing to
// switch, so the row is not drawn — a switch with one position is furniture.

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import Omnuv 1.0

RowLayout {
    id: bar
    spacing: Theme.spacingLoose

    readonly property var estate: Omnuv.estate
    readonly property var members: estate.members.data

    // "2 members · you are the owner". A part whose source has not been read
    // is left out rather than guessed.
    function detail() {
        var parts = []
        if (members !== undefined && members.length !== undefined) {
            parts.push(members.length === 1 ? qsTr("1 member") : qsTr("%1 members").arg(members.length))
        }
        if (estate.organizationName !== "") {
            parts.push(estate.owner ? qsTr("you are the owner") : qsTr("you are a member"))
        }
        return parts.join(" \u00B7 ")
    }

    // Whose cloud: an initial on the accent's wash, the name, and the two
    // facts about the person's place in it.
    Rectangle {
        visible: bar.estate.organizationName !== ""
        implicitWidth: 36
        implicitHeight: 36
        radius: Theme.radiusOverlay
        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.22)

        Label {
            anchors.centerIn: parent
            text: bar.estate.organizationName.charAt(0).toUpperCase()
            font.family: Theme.textFamily
            font.pixelSize: Theme.subtitleSize
            font.weight: Theme.strongWeight
        }
    }

    ColumnLayout {
        visible: bar.estate.organizationName !== ""
        spacing: 0
        Layout.maximumWidth: 280

        Label {
            Layout.fillWidth: true
            text: bar.estate.organizationName
            elide: Label.ElideRight
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            font.weight: Theme.strongWeight
        }
        Label {
            Layout.fillWidth: true
            text: bar.detail()
            visible: text !== ""
            elide: Label.ElideRight
            font.family: Theme.textFamily
            font.pixelSize: Theme.captionSize
            opacity: 0.6
        }
    }

    // A hairline between whose it is and which part of it is in view.
    Rectangle {
        visible: segments.visible
        implicitWidth: 1
        implicitHeight: 24
        color: Theme.strokeCard
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

    Item {
        Layout.fillWidth: true
    }

    ToolButton {
        text: Theme.iconsInstalled ? Theme.icon.refresh : Accessible.name
        font.family: Theme.iconsInstalled ? Theme.iconFamily : Theme.textFamily
        font.pixelSize: Theme.bodySize
        Accessible.name: qsTr("Refresh")
        ToolTip.visible: hovered
        ToolTip.text: Accessible.name
        onClicked: Omnuv.refresh(true)
    }

    ToolButton {
        id: more
        text: Theme.iconsInstalled ? Theme.icon.more : Accessible.name
        font.family: Theme.iconsInstalled ? Theme.iconFamily : Theme.textFamily
        font.pixelSize: Theme.bodySize
        Accessible.name: qsTr("More")
        ToolTip.visible: hovered
        ToolTip.text: Accessible.name
        onClicked: moreMenu.open()

        Menu {
            id: moreMenu
            y: more.height

            MenuItem {
                text: qsTr("Sign out")
                onTriggered: Omnuv.signOut()
            }
        }
    }
}
