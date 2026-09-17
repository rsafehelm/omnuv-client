// Omnuv: the top bar — whose cloud this is, which project is in view, and the
// window's two actions. What just happened is said in the messages panel at
// the bottom, not here. One row, no frame: it is the answer to "what am I
// looking at", so it never scrolls away and never competes with the machines.
//
// The project switch is a row of segments rather than a menu: a person with
// three projects should see all three names, and the selected one is the only
// thing on the bar drawn in the accent. With one project there is nothing to
// switch, so the row is not drawn — a switch with one position is furniture.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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

    // Whose cloud: an initial on the accent, lit from one corner like the
    // pictures on the cards below it, then the name and the two facts about
    // the person's place in it. The tile is always a deep colour, so the
    // initial is always white — see `Theme.tileFrom`.
    Rectangle {
        visible: bar.estate.organizationName !== ""
        implicitWidth: 40
        implicitHeight: 40
        radius: Theme.radiusOverlay
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Theme.tileFrom }
            GradientStop { position: 1; color: Theme.tileTo }
        }

        Label {
            anchors.centerIn: parent
            text: bar.estate.organizationName.charAt(0).toUpperCase()
            color: Theme.onTile
            font.family: Theme.displayFamily
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

            // Windows' SelectorBar: every name plain, and a short rule in
            // the accent under the one in view. A filled button here was the
            // loudest thing in the window, louder than Play, for a choice
            // most people make once.
            ToolButton {
                id: segment
                readonly property bool selected: modelData === Omnuv.projectId
                text: Omnuv.projectNames[index]
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                font.weight: selected ? Theme.strongWeight : Theme.regularWeight
                opacity: selected || hovered ? 1 : 0.78
                Accessible.name: selected ? qsTr("%1, the project in view").arg(text) : text
                onClicked: Omnuv.selectProject(modelData)

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 1
                    width: segment.selected ? 16 : 0
                    height: 3
                    radius: 1.5
                    color: Theme.accent
                    Behavior on width {
                        NumberAnimation {
                            duration: Theme.durationFast
                            easing.type: Easing.Bezier
                            easing.bezierCurve: Theme.easeEntrance
                        }
                    }
                }
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
