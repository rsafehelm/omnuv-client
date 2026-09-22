// Omnuv: the first thing a person sees.
//
// Two states, and no third. Either this device is signed in and shows the
// machines that person rents, or it is not and shows a code to approve in a
// browser. No password is ever typed here.
//
// Connect does the rest with upstream's own machinery: a streamed machine is
// added by its private name and streamed, an ordinary one gets a terminal.
// Nothing about streaming is reimplemented here.
//
// **Play goes to the stream, not to a grid.** Upstream's app grid was the
// screen between the two until 15 September 2026, and for a machine that
// streams one thing it is a screen with one box on it. The machine already
// says what it streams — Core sends `stream_app` — so the grid becomes what it
// should always have been: the answer to *choose what to stream*, on the card's
// menu, for the rare machine with several. `CliStartStreamSegue` proves the
// path: the CLI has started a named application without the grid all along.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

import ComputerModel 1.0
import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0

import Omnuv 1.0
import "estate.js" as Estate

Item {
    id: root
    focus: true
    Keys.onEscapePressed: function(event) {
        if (activeTarget || pendingTarget || delivering) root.cancelConnection()
        else event.accepted = false
    }

    // The toolbar shows this, so it should say where a person actually is.
    objectName: Omnuv.signedIn ? qsTr("Machines") : qsTr("Sign in to Omnuv")

    // One operation at a time, identified independently of model row order.
    property var activeTarget: null
    property var pendingTarget: null
    property bool delivering: false
    property bool chooseApp: false
    property string justPaired: ""
    property var launchApps: null
    property ComputerModel hosts: createHosts()

    StackView.onActivated: { Omnuv.refresh(true); Omnuv.tunnel.watch(true) }
    StackView.onDeactivating: Omnuv.tunnel.watch(false)

    function createHosts() {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', root, '')
        model.initialize(ComputerManager)
        Omnuv.watchPairing(ComputerManager)
        return model
    }

    function cancelConnection() {
        if (delivering) ComputerManager.cancelPairing(pairing.host)
        settle.stop()
        appWait.stop()
        activeTarget = null
        chooseApp = false
        pairing.close()
        Omnuv.finishPairing()
        // Keep slots until the cancelled pairing worker or pending add reports
        // completion, so its callback cannot finish a newer attempt.
    }

    function validTarget(target, starting) {
        if (!target || (!starting && !activeTarget) || Omnuv.targetRow(target) < 0) {
            cancelConnection()
            return false
        }
        return true
    }

    Repeater {
        id: hostProbe
        model: hosts
        delegate: Item {
            visible: false
            readonly property bool hostOnline: model.online
            readonly property bool hostPaired: model.paired
            readonly property bool hostStatusUnknown: model.statusUnknown
        }
    }
    Repeater {
        id: appProbe
        delegate: Item {
            visible: false
            readonly property string appName: model.name
        }
    }
    function hostIndexFor(host) { return Omnuv.hostRowFor(ComputerManager, host) }
    function appIndexFor(name) {
        for (var i = 0; i < appProbe.count; i++) {
            var item = appProbe.itemAt(i)
            if (item && item.appName.toLowerCase() === name.toLowerCase()) return i
        }
        return -1
    }

    Connections {
        target: ComputerManager
        function onComputerAddCompleted(success, detectedPortBlocking) {
            if (!root.pendingTarget) return
            var target = root.pendingTarget
            root.pendingTarget = null
            if (!root.validTarget(target)) return
            if (!success) {
                root.activeTarget = null
                message.show(detectedPortBlocking
                    ? qsTr("This network is blocking the ports streaming needs. Try a different network.")
                    : qsTr("No streaming host answered at %1. Check your Client VPN and the machine's console.").arg(target.host))
                return
            }
            root.openHost(target)
        }
    }

    Timer {
        id: settle
        property var target: null
        property int tries: 0
        interval: 500
        repeat: true
        onTriggered: {
            if (!root.validTarget(target)) return
            var index = root.hostIndexFor(target.host)
            var item = index >= 0 ? hostProbe.itemAt(index) : null
            if (item && item.hostOnline) {
                stop()
                root.openHost(target)
            } else if (++tries > 30) {
                stop()
                root.activeTarget = null
                message.show(item && item.hostStatusUnknown
                    ? qsTr("This device cannot tell whether %1 is up. Check your Client VPN.").arg(target.host)
                    : qsTr("%1 is not answering yet. It may still be starting.").arg(target.host))
            }
        }
        function watch(t) { target = t; tries = 0; start() }
    }

    function openHost(target) {
        if (!validTarget(target)) return
        var row = Omnuv.targetRow(target)
        var index = hostIndexFor(target.host)
        var item = index >= 0 ? hostProbe.itemAt(index) : null
        if (!item || !item.hostOnline) { settle.watch(target); return }
        var alreadyPaired = target.host === justPaired
        justPaired = ""
        if (!item.hostPaired && !alreadyPaired) {
            if (delivering) return
            var pin = hosts.generatePinString()
            pairing.machine = Omnuv.machines.nameAt(row)
            pairing.host = target.host
            pairing.pin = pin
            pairing.target = target
            pairing.why = ""
            pairing.detail = ""
            delivering = true
            hosts.pairComputer(index, pin)
            Omnuv.deliverPin(target, pin)
            return
        }
        if (chooseApp) { chooseApp = false; openAppGrid(target); return }
        launch(target)
    }

    function pairingFinished(address, error) {
        if (!delivering || address !== pairing.host) return
        var target = pairing.target
        pairing.target = null
        delivering = false
        pairing.close()
        Omnuv.finishPairing()
        if (!validTarget(target)) return
        if (error !== undefined) {
            activeTarget = null
            message.show(error)
            return
        }
        justPaired = address
        openHost(target)
    }

    function openAppGrid(target) {
        if (!validTarget(target)) return
        var index = hostIndexFor(target.host)
        if (index < 0) { cancelConnection(); return }
        var component = Qt.createComponent("qrc:/gui/AppView.qml")
        var appView = component.createObject(stackView, {
            "computerIndex": index,
            "objectName": Omnuv.machines.nameAt(Omnuv.targetRow(target))
        })
        activeTarget = null
        stackView.push(appView)
    }

    function launch(target) {
        if (!validTarget(target)) return
        var index = hostIndexFor(target.host)
        if (index < 0) { cancelConnection(); return }
        if (launchApps) { appProbe.model = null; launchApps.destroy() }
        launchApps = Qt.createQmlObject('import AppModel 1.0; AppModel {}', root, '')
        launchApps.initialize(ComputerManager, index, true)
        appProbe.model = launchApps
        appWait.target = target
        appWait.tries = 0
        appWait.start()
    }

    Timer {
        id: appWait
        property var target: null
        property int tries: 0
        interval: 500
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            if (!root.validTarget(target)) return
            var row = Omnuv.targetRow(target)
            if (appProbe.count > 0) {
                stop()
                var want = Omnuv.machines.streamAppAt(row)
                var app = root.appIndexFor(want)
                if (app >= 0) root.startStream(target, app)
                else {
                    message.show(qsTr("%1 is not offering %2 right now. Choose what to stream.")
                        .arg(Omnuv.machines.nameAt(row)).arg(want))
                    root.openAppGrid(target)
                }
            } else if (++tries > 30) {
                stop()
                root.activeTarget = null
                message.show(qsTr("%1 answered, but has not said what it can stream yet.").arg(target.host))
            }
        }
    }

    function startStream(target, appIndex) {
        if (!validTarget(target)) return
        var row = Omnuv.targetRow(target)
        var component = Qt.createComponent("qrc:/omnuv/OmnuvSegue.qml")
        var segue = component.createObject(stackView, {
            "session": launchApps.createSessionForApp(appIndex),
            "appName": Omnuv.machines.streamAppAt(row),
            "machineName": Omnuv.machines.nameAt(row),
            "machineHost": target.host,
            "machineUser": Omnuv.machines.userAt(row)
        })
        segue.retryRequested.connect(function() {
            stackView.pop()
            root.connectTarget(target, false)
        })
        activeTarget = null
        stackView.push(segue)
    }

    function openTerminalFor(row) {
        var host = Omnuv.machines.hostAt(row)
        var user = Omnuv.machines.userAt(row)
        if (!Omnuv.openTerminal(host, user))
            message.show(qsTr("No terminal could be started. Connect with:\n\nssh %1@%2").arg(user).arg(host))
    }
    function connectTo(row, choose) { connectTarget(Omnuv.connectionTarget(row), choose === true) }
    function connectTarget(target, choose) {
        if (activeTarget || pendingTarget || delivering) return
        if (!validTarget(target, true)) return
        activeTarget = target
        chooseApp = choose
        var row = Omnuv.targetRow(target)
        if (!Omnuv.machines.streamedAt(row)) { openTerminalFor(row); activeTarget = null; return }
        if (hostIndexFor(target.host) >= 0) { openHost(target); return }
        pendingTarget = target
        ComputerManager.addNewHostManually(target.host)
    }

    Connections {
        target: Omnuv
        function onConnectionContextChanged() { root.cancelConnection(); networkMove.close(); revokeEnrollment.close() }
        function onEnrollmentChanged() {
            if (Omnuv.networkMovePrompt !== "") networkMove.open()
            else networkMove.close()
        }
        function onHostPairingFinished(address, error) { root.pairingFinished(address, error) }
        function onPairingFailed(why, detail) {
            if (!root.validTarget(pairing.target)) return
            pairing.why = why
            pairing.detail = detail
            pairing.open()
        }
    }
    Connections {
        target: Omnuv.machines
        function onCountChanged() {
            if (root.activeTarget && Omnuv.targetRow(root.activeTarget) < 0) root.cancelConnection()
        }
    }

    // ---------------------------------------------------------------- dialogs

    Dialog {
        id: networkMove
        objectName: "networkMove"
        anchors.centerIn: parent
        width: Math.min(root.width - 80, 560)
        modal: true
        title: qsTr("Move this device to another network")
        standardButtons: Dialog.Yes | Dialog.Cancel
        contentItem: Label { text: Omnuv.networkMovePrompt; wrapMode: Text.WordWrap }
        onAccepted: Omnuv.confirmNetworkMove()
        onRejected: Omnuv.cancelNetworkMove()
    }
    Dialog {
        id: revokeEnrollment
        anchors.centerIn: parent
        width: Math.min(root.width - 80, 520)
        modal: true
        title: qsTr("Revoke the saved enrollment")
        standardButtons: Dialog.Yes | Dialog.Cancel
        contentItem: Label {
            text: qsTr("Revoke this saved attempt and disconnect it if it joined? Any other network membership on this device is kept. You can join again after cleanup is confirmed.")
            wrapMode: Text.WordWrap
        }
        onAccepted: Omnuv.revokePendingEnrollment()
    }

    // **A fallback, not a step.** This opens only when the code could not be
    // handed over, and it leads with the reason rather than with the number,
    // because the number is no longer the thing a person is here to do — it is
    // what they have been left with.
    //
    // The code that says *which* step failed is folded away. A buyer who reads
    // "GET /api/pin returned 401" learns nothing and remembers it as the
    // product being broken; a buyer who can open it when somebody asks them to
    // has lost nothing.
    Dialog {
        id: pairing
        property string machine
        property string host
        property string pin

        // Which machine this attempt was for, so a completion can carry on
        // where it left off.
        property var target: null

        // One sentence for the person, and the machine-shaped remainder.
        property string why
        property string detail

        anchors.centerIn: parent
        width: Math.min(root.width - 80, 460)
        modal: true
        standardButtons: Dialog.Close
        title: qsTr("Finish pairing with %1").arg(machine)

        onClosed: disclosure.shown = false
        onRejected: root.cancelConnection()

        ColumnLayout {
            width: parent.width
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: pairing.why
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.bodySize
                lineHeight: Theme.bodyLineHeight
                lineHeightMode: Text.FixedHeight
            }

            Label {
                Layout.fillWidth: true
                // Five minutes is the machine's own budget, not a guess:
                // Sunshine holds a pairing request open for
                // PAIRING_SESSION_TIMEOUT, `src/nvhttp.h:68` at the tag the
                // platform pins.
                text: qsTr("Enter this code on the machine’s setup page while pairing is pending. " +
                           "The pairing request expires after five minutes.")
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.bodySize
                opacity: 0.7
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: pairing.pin
                font.pixelSize: Theme.titleSize
                font.letterSpacing: 6
                font.family: Theme.monoFamily
            }

            Button {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Open the machine's setup page")
                onClicked: Qt.openUrlExternally("https://" + pairing.host + ":47990")
            }

            Button {
                id: disclosure
                property bool shown: false
                Layout.alignment: Qt.AlignHCenter
                flat: true
                visible: pairing.detail !== ""
                text: shown ? qsTr("Hide technical detail") : qsTr("Technical detail")
                onClicked: shown = !shown
            }

            Label {
                Layout.fillWidth: true
                visible: disclosure.shown
                text: pairing.detail
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.captionSize
                font.family: Theme.monoFamily
                opacity: 0.6
            }
        }
    }

    // The six things a buyer decides about a stream. One instance for the
    // view rather than one per card: the settings are the device's, not the
    // machine's, so a sheet per card would be N copies of one answer.
    StreamSettingsSheet {
        id: streamSettings
        onAdvancedRequested: stackView.push("qrc:/gui/SettingsView.qml")
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
        spacing: Theme.padding
        visible: !Omnuv.signedIn

        // Whose window this is, before anything is asked.
        Image {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: Theme.spacing
            source: "omnuv.svg"
            sourceSize: Qt.size(64, 64)
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Sign in to Omnuv")
            font.family: Theme.displayFamily
            font.pixelSize: Theme.titleSize
            font.weight: Theme.strongWeight
            horizontalAlignment: Text.AlignHCenter
        }

        Label {
            Layout.fillWidth: true
            visible: Omnuv.userCode === ""
            text: qsTr("Your machines appear here once this device is approved. " +
                       "You will not be asked for a password.")
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            opacity: 0.78
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
            highlighted: true
            implicitWidth: Math.max(140, implicitContentWidth + leftPadding + rightPadding)
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

            // The code, a character to a tile: it is read aloud, typed on a
            // phone and compared by eye, and a tile per character is how every
            // one of those goes right. A separator Core put in the code is
            // kept as one, between the tiles.
            Row {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.spacing
                Layout.bottomMargin: Theme.spacing
                spacing: Theme.spacingTight + 2
                Accessible.role: Accessible.StaticText
                Accessible.name: Omnuv.userCode

                Repeater {
                    model: Omnuv.userCode.split("")

                    Rectangle {
                        readonly property bool separator: modelData === "-" || modelData === " "
                        width: separator ? 12 : 44
                        height: 56
                        radius: Theme.radiusControl
                        color: separator ? "transparent" : Theme.fillCard
                        border.width: separator ? 0 : 1
                        border.color: Theme.strokeCardHover

                        Label {
                            anchors.centerIn: parent
                            text: modelData
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.titleSize
                            font.weight: Theme.strongWeight
                            opacity: parent.separator ? 0.5 : 1
                        }
                    }
                }
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
    //
    // One surface. The top bar, at most two notices, then the estate: the
    // project and its pulse over a grid of machine cards, and a quiet rail of
    // everything else beside it. The machines are the only cards, because
    // they are the only things a person acts on; the rail is text. Below
    // `wideEnough` there is no room for a rail, so the same rail follows the
    // grid instead of standing beside it.

    // A line that says one thing and offers one action: an edge in the tone,
    // a faint wash of it, and no frame. Windows calls this an InfoBar.
    component Notice: Rectangle {
        id: notice
        property alias text: noticeText.text
        property alias actionText: noticeAction.text
        property bool actionEnabled: true
        property color tone: Theme.fillCaution
        signal action()

        Layout.fillWidth: true
        implicitHeight: Math.max(40, noticeRow.implicitHeight + 2 * Theme.spacing)
        radius: Theme.radiusControl
        color: Qt.rgba(tone.r, tone.g, tone.b, 0.12)

        border.width: 1
        border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.30)

        RowLayout {
            id: noticeRow
            anchors.fill: parent
            anchors.leftMargin: Theme.padding
            anchors.rightMargin: Theme.spacing
            spacing: Theme.spacingLoose

            // The InfoBar's own severity glyph, in its tone; the sentence
            // beside it is what says what is wrong.
            Glyph {
                icon: Theme.icon.warning
                size: 16
                color: notice.tone
            }

            Label {
                id: noticeText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
            }

            Button {
                id: noticeAction
                visible: text !== ""
                enabled: notice.actionEnabled
                flat: true
                onClicked: notice.action()
            }
        }
    }

    // Nothing to show yet, said the same way wherever it is said: the lit
    // tile the organization wears, a title, a sentence, and at most one
    // action. The window's own colours, where a stock illustration would be
    // somebody else's.
    component EmptyState: ColumnLayout {
        id: empty
        property string icon
        property alias title: emptyTitle.text
        property alias text: emptyText.text
        property alias actionText: emptyAction.text
        signal action()

        width: Math.min(parent.width, 460)
        spacing: Theme.spacing

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: Theme.spacingLoose
            implicitWidth: 72
            implicitHeight: 72
            radius: 20
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Theme.tileFrom }
                GradientStop { position: 1; color: Theme.tileTo }
            }

            Glyph {
                anchors.centerIn: parent
                icon: empty.icon
                size: 32
                color: Theme.onTile
            }
        }
        Label {
            id: emptyTitle
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            font.family: Theme.displayFamily
            font.pixelSize: Theme.subtitleSize
            font.weight: Theme.strongWeight
        }
        Label {
            id: emptyText
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            opacity: 0.78
        }
        Button {
            id: emptyAction
            visible: text !== ""
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spacingLoose
            highlighted: true
            onClicked: empty.action()
        }
    }

    // The colour this window's controls are drawn on, handed to `Theme` so
    // the cards choose their fill from the surface they sit on. A Control,
    // because a Control is what carries the palette the style resolved.
    Control {
        id: surfaceProbe
        visible: false
    }
    Binding {
        target: Theme
        property: "surface"
        value: surfaceProbe.palette.window
    }
    // And the rest of it, for high contrast: see `Theme.probe`.
    Binding {
        target: Theme
        property: "probe"
        value: surfaceProbe.palette
    }

    // **Mica, where Windows will draw it.** The window stops painting its own
    // background and DWM's backdrop — the person's wallpaper, blurred and
    // tinted to the theme — is what the cards and the rail sit on, which is
    // what Microsoft's translucent card and layer fills were published for.
    // Only when `appearance.backdrop` says every part of it is in place (see
    // `OmnuvAppearance::applyBackdrop`); otherwise this binding is inactive
    // and the style's own `palette.window` stays, exactly as before.
    //
    // The style declares `color: window.palette.window` on its
    // ApplicationWindow, so this has to be a Binding on the same property
    // rather than an assignment somewhere: an assignment would be overwritten
    // the next time the palette moved.
    Binding {
        target: root.Window.window
        property: "color"
        value: "transparent"
        when: root.Window.window !== null && Omnuv.appearance.backdrop
    }

    // Light behind the top of the window, signed in or not.
    Aurora {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Math.min(parent.height, 360)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.padding + Theme.spacing
        spacing: Theme.padding
        visible: Omnuv.signedIn

        OrganizationBand {
            Layout.fillWidth: true
        }

        Notice {
            objectName: "credentialWarning"
            visible: Omnuv.credentialWarning !== ""
            text: Omnuv.credentialWarning
        }
        Notice {
            objectName: "readProblem"
            visible: Omnuv.readProblem !== ""
            text: Omnuv.readProblem
            actionText: qsTr("Try again")
            onAction: Omnuv.retryReads()
        }

        // Whether this device is on the project network at all. Shown before
        // the machines, because "Connect does nothing" is nearly always this.
        Notice {
            visible: !Omnuv.tunnel.connected || Omnuv.tunnel.operationError !== ""
            text: Omnuv.tunnel.operationError !== "" ? Omnuv.tunnel.operationError : Omnuv.tunnel.state
            actionText: Omnuv.tunnel.busy ? qsTr("Joining…") : qsTr("Join this device")
            actionEnabled: Omnuv.tunnel.available && !Omnuv.tunnel.busy && Omnuv.signedIn
            onAction: Omnuv.tunnel.join()
        }

        Notice {
            objectName: "enrollmentRecovery"
            visible: Omnuv.enrollmentRecovery !== ""
            text: Omnuv.enrollmentRecovery
            actionText: Omnuv.enrollmentCleanupBusy ? qsTr("Revoking…") : qsTr("Revoke saved enrollment")
            actionEnabled: !Omnuv.enrollmentCleanupBusy
            onAction: revokeEnrollment.open()
        }

        // A refresh that failed. What was shown stays, dimmed where it is
        // drawn; this says why, once for the whole screen.
        Notice {
            visible: estateHeader.stale.length > 0
            text: estateHeader.stale.length === 0 ? ""
                  : qsTr("Showing %1 \u2014 could not refresh: %2")
                    .arg(Qt.formatTime(estateHeader.oldestStale(), "HH:mm"))
                    .arg(estateHeader.stale[0].problem)
            actionText: qsTr("Try again")
            onAction: Omnuv.estate.retryAll()
        }

        // An organization with no project: nothing project-scoped exists to
        // read, so the window says what to do rather than showing a grid and
        // a rail that would wait for ever.
        Item {
            visible: Omnuv.noProject
            Layout.fillWidth: true
            Layout.fillHeight: true

            EmptyState {
                anchors.centerIn: parent
                icon: Theme.icon.cloud
                title: qsTr("No project yet")
                text: qsTr("This organization has no project to show. Signing in to the Omnuv console sets up a new workspace, and this window fills in once it exists.")
                actionText: qsTr("Check again")
                onAction: Omnuv.refresh(true)
            }
        }

        RowLayout {
            id: estateBody
            visible: !Omnuv.noProject
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.padding

            readonly property bool wideEnough: width >= 900

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.spacingLoose

                EstateHeader {
                    id: estateHeader
                    Layout.fillWidth: true
                    now: machineList.now
                }

                GridView {
                    id: machineList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: Omnuv.machines
                    clip: true
                    focus: true
                    keyNavigationWraps: true
                    boundsBehavior: Flickable.StopAtBounds

                    // As many columns of 280 or more as fit, then shared out,
                    // so a row always reaches the edge. Every cell is one
                    // height: a grid whose rows do not line up reads as a pile.
                    readonly property int gap: Theme.spacingLoose
                    readonly property int columns: Math.max(1, Math.floor(width / 292))
                    cellWidth: Math.floor(width / columns)
                    // A picture, a name, a state and a button, with room for a
                    // launch under way and for two lines of Core's reason.
                    cellHeight: 292
                    // The shadows reach past the cards; the first row's must
                    // not be cut by the grid's own edge.
                    topMargin: 4

                    ScrollBar.vertical: ScrollBar {}

                    // Seconds are shown — "observed 8s ago", "42s" — so
                    // seconds have to pass. One timer for the whole screen
                    // rather than one per card, and it stops when nobody is
                    // looking.
                    property double now: Date.now()

                    Timer {
                        interval: 1000
                        repeat: true
                        running: machineList.visible && machineList.Window.active
                        onTriggered: machineList.now = Date.now()
                    }

                    // Nothing rented yet — said only once the list has actually
                    // been read, and not while something is waiting for
                    // capacity, which is drawn as a card of its own.
                    EmptyState {
                        visible: Omnuv.machines.loaded && Omnuv.machines.count === 0 && estateHeader.waiting.length === 0
                        x: (machineList.width - width) / 2
                        y: Math.max(0, (machineList.height - height) / 2 - 24)
                        icon: Theme.icon.game
                        title: qsTr("No machines yet")
                        text: qsTr("Rent one in the Omnuv console and it appears here, ready to play.")
                    }

                    // The cards arrive, one after another, rising a little as
                    // they do — Windows' own entrance, and its own curve. Once:
                    // a refresh updates the cards where they stand
                    // (`MachineModel::replace`), so this runs when the list is
                    // first read or is a different list, not every fifteen
                    // seconds. Under reduced motion every duration is zero and
                    // the cards are simply there.
                    populate: Transition {
                        id: arrive
                        SequentialAnimation {
                            PropertyAction { property: "opacity"; value: 0 }
                            PauseAnimation { duration: Theme.motion ? Math.min(arrive.ViewTransition.index, 6) * 45 : 0 }
                            ParallelAnimation {
                                NumberAnimation { property: "opacity"; to: 1; duration: Theme.durationNormal }
                                NumberAnimation {
                                    property: "y"
                                    from: arrive.ViewTransition.destination.y + 18
                                    to: arrive.ViewTransition.destination.y
                                    duration: Theme.durationSlow
                                    easing.type: Easing.Bezier
                                    easing.bezierCurve: Theme.easeEntrance
                                }
                            }
                        }
                    }

                    // Until the machines have been read: the shape of what is
                    // coming, breathing, in the places the cards will take —
                    // Docker Desktop draws its empty list the same way. Three,
                    // because that is a row; gone the moment the list lands.
                    Row {
                        visible: !Omnuv.machines.loaded
                        z: 2

                        Repeater {
                            model: Omnuv.machines.loaded ? 0 : Math.min(3, machineList.columns)

                            Item {
                                width: machineList.cellWidth
                                height: machineList.cellHeight

                                Rectangle {
                                    width: parent.width - machineList.gap
                                    height: parent.height - machineList.gap
                                    radius: Theme.radiusCard
                                    color: Theme.fillCard
                                    border.width: 1
                                    border.color: Theme.strokeCard

                                    Column {
                                        x: Theme.padding
                                        y: 64 + Theme.padding
                                        width: parent.width - 2 * Theme.padding
                                        spacing: Theme.spacingLoose

                                        Repeater {
                                            model: [0.55, 0.8, 0.4]

                                            Rectangle {
                                                width: parent.width * modelData
                                                height: index === 0 ? 20 : 12
                                                radius: Theme.radiusControl
                                                color: Theme.fillSubtle
                                            }
                                        }
                                    }

                                    Rectangle {
                                        x: 1
                                        y: 1
                                        width: parent.width - 2
                                        height: 64
                                        topLeftRadius: Theme.radiusCard - 1
                                        topRightRadius: Theme.radiusCard - 1
                                        color: Theme.fillSubtle
                                    }

                                    SequentialAnimation on opacity {
                                        // Only while it can be seen: an
                                        // animation on a hidden item still
                                        // ticks, for ever, with no project.
                                        running: Theme.motion && parent.visible
                                        loops: Animation.Infinite
                                        NumberAnimation { from: 1; to: 0.45; duration: 700; easing.type: Easing.InOutSine }
                                        NumberAnimation { from: 0.45; to: 1; duration: 700; easing.type: Easing.InOutSine }
                                    }
                                }
                            }
                        }
                    }

                    delegate: MachineCard {
                        width: machineList.cellWidth
                        height: machineList.cellHeight
                        gap: machineList.gap
                        now: machineList.now

                        onPrimaryActivated: root.connectTo(index)
                        onTerminalRequested: root.openTerminalFor(index)
                        onChooseAppRequested: {
                            root.connectTo(index, true)
                        }
                        onSettingsRequested: streamSettings.open()
                    }

                    // After the machines: what is waiting for capacity, as
                    // cards that are not machines yet — a dashed outline, the
                    // reason in Core's words, and when it gives up. Then, on a
                    // narrow window, the rail.
                    footer: Column {
                        width: machineList.width
                        spacing: Theme.padding

                        Flow {
                            width: parent.width

                            Repeater {
                                model: estateHeader.waiting

                                Item {
                                    width: machineList.cellWidth
                                    height: machineList.cellHeight

                                    Canvas {
                                        id: outline
                                        x: 0.5
                                        y: 0.5
                                        width: parent.width - machineList.gap - 1
                                        height: parent.height - machineList.gap - 1
                                        onWidthChanged: requestPaint()
                                        onHeightChanged: requestPaint()
                                        onPaint: {
                                            var ctx = getContext("2d")
                                            ctx.reset()
                                            ctx.strokeStyle = Theme.fillNeutral
                                            ctx.lineWidth = 1
                                            ctx.setLineDash([4, 3])
                                            ctx.beginPath()
                                            ctx.roundedRect(0, 0, width, height, Theme.radiusCard, Theme.radiusCard)
                                            ctx.stroke()
                                        }
                                    }

                                    ColumnLayout {
                                        x: Theme.padding
                                        y: Theme.padding
                                        width: outline.width - 2 * Theme.padding
                                        height: outline.height - 2 * Theme.padding
                                        spacing: Theme.spacing

                                        // The place a machine's picture will
                                        // be, holding the one thing known
                                        // about it so far: it is waiting.
                                        Rectangle {
                                            Layout.bottomMargin: Theme.spacing
                                            implicitWidth: 36
                                            implicitHeight: 36
                                            radius: Theme.radiusOverlay
                                            color: Theme.fillSubtle
                                            visible: Theme.iconsInstalled

                                            Glyph {
                                                anchors.centerIn: parent
                                                icon: Theme.icon.waiting
                                                size: 18
                                                opacity: 0.8
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Waiting for capacity")
                                            font.family: Theme.displayFamily
                                            font.pixelSize: Theme.subtitleSize
                                            font.weight: Theme.strongWeight
                                            elide: Label.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.waiting_on
                                            wrapMode: Text.WordWrap
                                            maximumLineCount: 3
                                            elide: Label.ElideRight
                                            font.family: Theme.textFamily
                                            font.pixelSize: Theme.bodySize
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.billing
                                            wrapMode: Text.WordWrap
                                            font.family: Theme.textFamily
                                            font.pixelSize: Theme.captionSize
                                            opacity: 0.6
                                        }
                                        Item {
                                            Layout.fillHeight: true
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("gives up %1").arg(Estate.ago(modelData.expires_at, machineList.now))
                                            font.family: Theme.textFamily
                                            font.pixelSize: Theme.captionSize
                                            opacity: 0.6
                                        }
                                    }
                                }
                            }
                        }

                        EstateRail {
                            visible: !estateBody.wideEnough
                            width: parent.width
                            now: machineList.now
                        }
                    }
                }
            }

            // The rail, on one quiet layer — LayerFillColorDefault, the
            // surface Windows lays content on above Mica — rather than a box
            // per section. It scrolls on its own when it is taller than the
            // window.
            Rectangle {
                visible: estateBody.wideEnough
                Layout.preferredWidth: 320
                Layout.fillHeight: true
                radius: Theme.radiusOverlay
                color: Theme.fillLayer
                border.width: 1
                border.color: Theme.strokeCard

                Flickable {
                    id: railScroller
                    anchors.fill: parent
                    anchors.margins: 1
                    contentHeight: sideRail.implicitHeight + 2 * Theme.padding
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    EstateRail {
                        id: sideRail
                        x: Theme.padding
                        y: Theme.padding
                        width: railScroller.width - 2 * Theme.padding
                        now: machineList.now
                    }
                }
            }
        }

        // What Omnuv has to say, read-only, folded to one line until opened.
        MessagesPanel {
            visible: !Omnuv.noProject
            Layout.fillWidth: true
            now: machineList.now
            hardwareDecoderUnavailable: runConfigChecks && !SystemProperties.hasHardwareAcceleration
                && StreamingPreferences.videoDecoderSelection !== StreamingPreferences.VDS_FORCE_SOFTWARE
            runningXWayland: SystemProperties.isRunningXWayland
        }
    }
}
