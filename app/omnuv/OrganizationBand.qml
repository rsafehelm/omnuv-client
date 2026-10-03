// Omnuv: the slim app bar — the brand, the project in view, and the account
// (the Instances redesign, section 4: "brand, project switcher, account; 52
// px; the web top bar's style"). It replaces the organization block, the
// project tabs, Refresh and the bar's own Deploy: the list refreshes itself,
// and Deploy lives in the page's header, as on the web.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omnuv 1.0

RowLayout {
    id: bar
    objectName: "appBar"
    spacing: Theme.spacingLoose
    implicitHeight: 52

    // The window owns the dialog; the bar only asks for it.
    signal switchDeploymentRequested()
    // Kept for callers: Deploy is in the page header now.
    signal deployRequested()

    readonly property var estate: Omnuv.estate

    Image {
        source: "omnuv.svg"
        sourceSize: Qt.size(28, 28)
        Layout.preferredWidth: 28
        Layout.preferredHeight: 28
        Accessible.ignored: true
    }
    Label {
        text: qsTr("Omnuv")
        font.family: Theme.displayFamily
        font.pixelSize: 16
        font.weight: Theme.strongWeight
    }

    Rectangle {
        implicitWidth: 1
        implicitHeight: 24
        color: Theme.strokeCard
    }

    // The ids are the values and the names the labels: two projects may
    // share a name, and choosing by name would choose the wrong network.
    ComboBox {
        id: projects
        objectName: "projectSwitcher"
        visible: Omnuv.projectIds.length > 1
        model: Omnuv.projectNames
        currentIndex: Omnuv.projectIds.indexOf(Omnuv.projectId)
        Accessible.name: qsTr("Project")
        implicitContentWidthPolicy: ComboBox.WidestText
        onActivated: function (i) { Omnuv.selectProject(Omnuv.projectIds[i]) }
        // Material's dropdown is a shadowed layer the software renderer does
        // not draw; ours is the popup surface (OmnuvSurface.qml).
        popup.background: OmnuvSurface {}
    }
    Label {
        visible: Omnuv.projectIds.length <= 1
        text: Omnuv.projectName
        font.pixelSize: Theme.bodySize
    }

    Item { Layout.fillWidth: true }

    ToolButton {
        id: account
        objectName: "accountMenu"
        text: bar.estate.organizationName !== "" ? bar.estate.organizationName : qsTr("Account")
        font.pixelSize: Theme.bodySize
        // A name that does not change with the organization, so the rig's
        // journey can find it before it knows which one is signed in; the
        // organization is still read out, as the description.
        Accessible.name: qsTr("Account")
        Accessible.description: text
        onClicked: accountMenu.open()

        Menu {
            id: accountMenu
            y: account.height
            background: OmnuvSurface { implicitWidth: 200 }

            // Another Omnuv deployment, without signing out of this one:
            // each keeps its own sign-in, so coming back needs none.
            MenuItem {
                text: qsTr("Switch deployment…")
                onTriggered: bar.switchDeploymentRequested()
            }
            MenuItem {
                text: qsTr("Sign out")
                onTriggered: Omnuv.signOut()
            }
        }
    }
}
