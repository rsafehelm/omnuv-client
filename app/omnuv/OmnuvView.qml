// Omnuv: the first thing a person sees.
//
// Two states, and no third. Either this device is signed in and shows the
// machines that person rents, or it is not and shows a code to approve in a
// browser. No password is ever typed here.
//
// Connect is deliberately dead in this version. Wiring it to the streaming
// path is the next step, and a button that looked live but did nothing would
// be worse than one that says why.

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import Omnuv 1.0

Item {
    id: root
    focus: true
    // The toolbar shows this, so it should say where a person actually is.
    objectName: Omnuv.signedIn ? qsTr("Machines") : qsTr("Sign in to Omnuv")

    StackView.onActivated: Omnuv.refresh()

    // ---------------------------------------------------------------- signed out

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 460)
        spacing: 16
        visible: !Omnuv.signedIn

        Label {
            Layout.fillWidth: true
            text: qsTr("Sign in to Omnuv")
            font.pointSize: 20
            horizontalAlignment: Text.AlignHCenter
        }

        Label {
            Layout.fillWidth: true
            visible: Omnuv.userCode === ""
            text: qsTr("Your machines appear here once this device is approved. " +
                       "You will not be asked for a password.")
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            opacity: 0.7
        }

        TextField {
            id: coreField
            Layout.fillWidth: true
            visible: Omnuv.userCode === ""
            text: Omnuv.coreUrl
            placeholderText: qsTr("https://your-omnuv-address")
            onEditingFinished: Omnuv.coreUrl = text
        }

        Button {
            Layout.alignment: Qt.AlignHCenter
            visible: Omnuv.userCode === ""
            enabled: !Omnuv.busy && coreField.text.trim() !== ""
            text: Omnuv.busy ? qsTr("Working…") : qsTr("Sign in")
            onClicked: {
                Omnuv.coreUrl = coreField.text
                Omnuv.signIn()
            }
        }

        // Waiting for the browser. The code is the whole interface here.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 12
            visible: Omnuv.userCode !== ""

            Label {
                Layout.fillWidth: true
                text: qsTr("Open this address and enter the code:")
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }

            Label {
                Layout.fillWidth: true
                text: Omnuv.verificationUri
                wrapMode: Text.WrapAnywhere
                horizontalAlignment: Text.AlignHCenter
                opacity: 0.7
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: Omnuv.userCode
                font.pointSize: 30
                font.letterSpacing: 4
                font.family: "monospace"
            }

            Button {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Cancel")
                onClicked: Omnuv.cancelSignIn()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: Omnuv.status !== ""
            text: Omnuv.status
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            opacity: 0.7
        }
    }

    // ----------------------------------------------------------------- signed in

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12
        visible: Omnuv.signedIn

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: Omnuv.status !== "" ? Omnuv.status
                                          : (Omnuv.machines.count === 1 ? qsTr("1 machine")
                                                                        : qsTr("%1 machines").arg(Omnuv.machines.count))
                elide: Label.ElideRight
                opacity: 0.7
            }

            Button {
                text: qsTr("Refresh")
                flat: true
                onClicked: Omnuv.refresh()
            }

            Button {
                text: qsTr("Sign out")
                flat: true
                onClicked: Omnuv.signOut()
            }
        }

        // Nothing rented yet. Says where to go rather than showing a blank.
        Label {
            Layout.fillWidth: true
            Layout.topMargin: 40
            visible: Omnuv.machines.count === 0
            text: qsTr("No machines yet. Rent one in the Omnuv console and it will appear here.")
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            opacity: 0.7
        }

        ListView {
            id: machineList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: Omnuv.machines.count > 0
            model: Omnuv.machines
            spacing: 8
            clip: true
            focus: true
            keyNavigationWraps: true

            delegate: ItemDelegate {
                width: machineList.width
                height: 84

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 14

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: model.name
                            font.pointSize: 14
                            elide: Label.ElideRight
                            Layout.fillWidth: true
                        }

                        Label {
                            // What it is, then where, then how it is doing.
                            text: (model.streamed ? model.streamApp : qsTr("Machine"))
                                  + " · " + model.region + " · " + model.status
                            opacity: 0.7
                            elide: Label.ElideRight
                            Layout.fillWidth: true
                        }

                        Label {
                            text: model.summary
                            opacity: 0.5
                            elide: Label.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    Button {
                        // Not yet: the streaming path is wired up next.
                        text: qsTr("Connect")
                        enabled: false
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Connecting from this application is not built yet. " +
                                           "Use %1 in the meantime.").arg(model.host)
                    }
                }
            }
        }
    }
}
