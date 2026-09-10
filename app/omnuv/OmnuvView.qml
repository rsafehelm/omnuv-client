// Omnuv: the first thing a person sees.
//
// Two states, and no third. Either this device is signed in and shows the
// machines that person rents, or it is not and shows a code to approve in a
// browser. No password is ever typed here.
//
// Connect does the rest with upstream's own machinery: a streamed machine is
// added by its private name and handed to the app view, an ordinary one gets
// a terminal. Nothing about streaming is reimplemented here.

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerModel 1.0
import ComputerManager 1.0

import Omnuv 1.0

Item {
    id: root
    focus: true

    // The toolbar shows this, so it should say where a person actually is.
    objectName: Omnuv.signedIn ? qsTr("Machines") : qsTr("Sign in to Omnuv")

    // The machine whose Connect was pressed while its host was still being
    // added. -1 when nothing is waiting.
    property int pendingRow: -1

    StackView.onActivated: Omnuv.refresh()

    // Upstream's own list of hosts. We do not keep our own: whether a host is
    // reachable and whether it is paired are things it already knows.
    property ComputerModel hosts: createHosts()

    function createHosts() {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', root, '')
        model.initialize(ComputerManager)
        return model
    }

    // A ComputerModel is a QAbstractListModel, which QML cannot index into
    // directly. A Repeater over it can, and costs one invisible item per host.
    Repeater {
        id: hostProbe
        model: hosts
        delegate: Item {
            visible: false
            readonly property string hostName: model.name
            readonly property bool hostOnline: model.online
            readonly property bool hostPaired: model.paired
        }
    }

    // Machines are named by their owner and the guest takes that name as its
    // hostname, which is what the streaming host reports. Matched without
    // regard to case, because the two ends disagree about it.
    function hostIndexFor(name) {
        for (var i = 0; i < hostProbe.count; i++) {
            var item = hostProbe.itemAt(i)
            if (item && item.hostName.toLowerCase() === name.toLowerCase()) {
                return i
            }
        }
        return -1
    }

    Connections {
        target: ComputerManager

        function onComputerAddCompleted(success, detectedPortBlocking) {
            if (root.pendingRow < 0) {
                return
            }
            var row = root.pendingRow
            root.pendingRow = -1

            if (!success) {
                // Two causes, and this device cannot tell them apart: it is
                // not on the Client VPN, or the machine is up but nothing is
                // streaming on it. Say both rather than guess wrong.
                message.show(qsTr("No streaming host answered at %1.\n\n" +
                                  "Either this device is not on your Client VPN, or the machine " +
                                  "has not finished setting itself up. Its console says which.")
                             .arg(Omnuv.machines.hostAt(row)))
                return
            }
            root.openHost(row)
        }
    }

    // Everything Connect does, once the host is known to be there.
    function openHost(row) {
        var index = hostIndexFor(Omnuv.machines.nameAt(row))
        if (index < 0) {
            message.show(qsTr("That machine answered but did not identify itself. Try again in a moment."))
            return
        }

        var item = hostProbe.itemAt(index)
        if (!item.hostOnline) {
            message.show(qsTr("%1 is not answering yet. Machines take a minute to finish starting.")
                         .arg(Omnuv.machines.nameAt(row)))
            return
        }

        if (!item.hostPaired) {
            // The PIN is made here, on this device, and typed into the
            // machine's own page. It cannot be done for a person: the machine
            // has never seen this number, which is the whole point of it.
            var pin = hosts.generatePinString()
            hosts.pairComputer(index, pin)
            pairing.machine = Omnuv.machines.nameAt(row)
            pairing.host = Omnuv.machines.hostAt(row)
            pairing.pin = pin
            pairing.open()
            return
        }

        var component = Qt.createComponent("qrc:/gui/AppView.qml")
        var appView = component.createObject(stackView, {
                                                 "computerIndex": index,
                                                 "objectName": Omnuv.machines.nameAt(row)
                                             })
        stackView.push(appView)
    }

    function connect(row) {
        if (!Omnuv.machines.streamedAt(row)) {
            var host = Omnuv.machines.hostAt(row)
            var user = Omnuv.machines.userAt(row)
            if (!Omnuv.openTerminal(host, user)) {
                message.show(qsTr("No terminal could be started. Connect with:\n\nssh %1@%2")
                             .arg(user).arg(host))
            }
            return
        }

        if (hostIndexFor(Omnuv.machines.nameAt(row)) >= 0) {
            openHost(row)
            return
        }

        // First time on this device: teach the streaming client the machine's
        // private name. `addNewHostManually` is the one upstream method this
        // application calls, and the whole integration rests on it.
        pendingRow = row
        ComputerManager.addNewHostManually(Omnuv.machines.hostAt(row))
    }

    // ---------------------------------------------------------------- dialogs

    Dialog {
        id: pairing
        property string machine
        property string host
        property string pin

        anchors.centerIn: parent
        width: Math.min(root.width - 80, 460)
        modal: true
        standardButtons: Dialog.Close
        title: qsTr("Pair with %1").arg(machine)

        ColumnLayout {
            width: parent.width
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: qsTr("Sign in to the machine's own page and enter this number. " +
                           "Its user name and password are on the machine's console, in the Omnuv " +
                           "console under Machines. Only needed once per device.")
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: pairing.pin
                font.pointSize: 30
                font.letterSpacing: 6
                font.family: "monospace"
            }

            Button {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Open the machine's page")
                onClicked: Qt.openUrlExternally("https://" + pairing.host + ":47990")
            }
        }
    }

    Dialog {
        id: message
        property alias text: messageLabel.text

        function show(what) {
            text = what
            open()
        }

        anchors.centerIn: parent
        width: Math.min(root.width - 80, 460)
        modal: true
        standardButtons: Dialog.Ok

        Label {
            id: messageLabel
            width: parent.width
            wrapMode: Text.WordWrap
        }
    }

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
                onClicked: if (model.ready) root.connect(index)

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
                        text: model.streamed ? qsTr("Play") : qsTr("Terminal")
                        enabled: model.ready
                        onClicked: root.connect(index)
                    }
                }
            }
        }
    }
}
