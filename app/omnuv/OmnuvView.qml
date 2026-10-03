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
import QtQml.Models

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
    objectName: Omnuv.signedIn ? qsTr("Instances") : qsTr("Sign in to Omnuv")

    // One operation at a time, identified independently of model row order.
    property var activeTarget: null
    property var pendingTarget: null
    property bool delivering: false
    property bool chooseApp: false
    property string justPaired: ""
    property var launchApps: null
    property ComputerModel hosts: createHosts()

    // ---- The Instances page's own state (the redesign, section 4) -------
    // The chip in view: one of Core's words, or "" for All.
    property string filter: ""
    // The sum of Core's prices, an estimate while nothing charges.
    property real estimate: 0
    // The one-time console password a deploy answered with, and whose.
    property string oneTimePassword: ""
    property string oneTimeFor: ""
    // **Recently removed** (section 4): "Removing <name> (<app>)" while Core
    // takes it away, then "<name> was removed" once the list no longer has
    // it. By id, for the session; the newest first, five at most.
    property var removed: []
    function noteRemoving(id, name, app) {
        var next = [{ id: id, name: name, app: app, gone: false }]
        for (var i = 0; i < removed.length && next.length < 5; ++i)
            if (removed[i].id !== id) next.push(removed[i])
        removed = next
    }
    function settleRemoved() {
        var listed = {}
        for (var r = 0; r < Omnuv.machines.count; ++r) listed[Omnuv.machines.idAt(r)] = true
        var next = [], changed = false
        for (var i = 0; i < removed.length; ++i) {
            var e = removed[i]
            if (!e.gone && Omnuv.machines.loaded && !listed[e.id]) {
                e = { id: e.id, name: e.name, app: e.app, gone: true }
                changed = true
            }
            next.push(e)
        }
        if (changed) removed = next
    }

    // A chip counts the words it stands for: Running with Ready, Installing
    // with Deploying, as the card tones them.
    function wordsOf(chip) {
        return chip === "Running" ? ["Running", "Ready"] : chip === "Installing" ? ["Installing", "Deploying"] : [chip]
    }
    function countOf(chip) {
        var counts = Omnuv.machines.statusCounts, n = 0, words = wordsOf(chip)
        for (var i = 0; i < words.length; ++i) n += counts[words[i]] || 0
        return n
    }
    // Which cards the chip shows, and the estimate, from the list as it is.
    function refilter() {
        var total = 0, words = wordsOf(filter)
        for (var i = 0; i < shown.items.count; ++i) {
            var item = shown.items.get(i)
            item.inMatching = filter === "" || words.indexOf(item.model.status) >= 0
            total += parseFloat(item.model.price) || 0
        }
        estimate = total
        // A chip whose last card went shows All again, never an empty page.
        if (filter !== "" && countOf(filter) === 0) { filter = ""; refilter() }
    }
    // What deleting it loses, one line each (section 5).
    function lossesOf(m) {
        var out = []
        if (m.hasApp) out.push(m.loses !== "" ? qsTr("%1, %2").arg(m.appName).arg(m.loses) : m.appName)
        out.push(qsTr("The instance's disk, %1 GiB").arg(m.diskGib))
        if (m.privateIp !== "") out.push(qsTr("Its address on your private network"))
        if (m.gpuModel !== "") out.push(qsTr("Its %1, back for others to rent").arg(m.gpuModel))
        return out
    }
    Connections {
        target: Omnuv.machines
        function onCountChanged() { root.refilter(); root.settleRemoved() }
    }
    Connections {
        target: Omnuv
        function onDeployed(instances) {
            for (var i = 0; i < instances.length; ++i) {
                if (instances[i].console_password) {
                    root.oneTimePassword = instances[i].console_password
                    root.oneTimeFor = instances[i].name
                    break
                }
            }
        }
        function onActionFinished(ok, text) { if (!ok) message.show(text) }
    }

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
    // A sentence names the machine, never its address: the address is
    // `<machine uuid>.<project uuid>.<domain>` since 3 October 2026.
    function machineName(target) {
        var row = target ? Omnuv.targetRow(target) : -1
        return row >= 0 ? Omnuv.machines.nameAt(row) : (target ? target.host : "")
    }
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
            if (root.abandonedAdds > 0) { root.abandonedAdds--; return }
            if (!root.pendingTarget) return
            var target = root.pendingTarget
            root.pendingTarget = null
            if (!root.validTarget(target)) return
            if (!success) {
                root.activeTarget = null
                message.show(detectedPortBlocking
                    ? qsTr("This network is blocking the ports streaming needs. Try a different network.")
                    : qsTr("No streaming host answered on %1. Check your Client VPN and the machine's console.").arg(root.machineName(target)))
                return
            }
            Omnuv.rememberStreamHost(ComputerManager, target)
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
                    ? qsTr("This device cannot tell whether %1 is up. Check your Client VPN.").arg(root.machineName(target))
                    : qsTr("%1 is not answering yet. It may still be starting.").arg(root.machineName(target)))
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
        // Spent only by a pairing that worked; any other ending gives the
        // login back for the next attempt (BUYER-11).
        Omnuv.finishPairing(error === undefined)
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
                message.show(qsTr("%1 answered, but has not said what it can stream yet.").arg(root.machineName(target)))
            }
        }
    }

    // **A changed resolution or frame rate restarts the desktop** (2 October
    // 2026). Sunshine sizes the rig's display when it launches an app, and a
    // Play while the app runs resumes it, so a new resolution never arrived:
    // the operator changed theirs and the rig stayed at 1280x720 through four
    // sessions. Each launch records the shape it asked for, per machine; a
    // Play whose shape differs, or is unknown, quits a running Desktop first
    // and launches afresh (recorded in QSettings by Omnuv, not QtCore's
    // Settings: Windows has no QtCore QML module). Only Desktop: quitting it ends the stream and
    // nothing on the machine, while quitting a game could lose its progress,
    // so any other running app is resumed as before.
    function streamShape() {
        return StreamingPreferences.width + "x" + StreamingPreferences.height + "@" + StreamingPreferences.fps
    }
    function startStream(target, appIndex) {
        if (!validTarget(target)) return
        var want = streamShape()
        var had = Omnuv.launchedShape(target.id)
        var running = launchApps.getRunningAppId() !== 0 ? launchApps.getRunningAppName() : ""
        if (running !== "Desktop" || had === want) {
            beginStream(target, appIndex, want)
            return
        }
        console.info("omnuv: play: the desktop runs at " + (had || "an unknown shape") + "; restarting it at " + want)
        message.show(qsTr("Restarting the desktop at %1×%2, %3 fps…")
                     .arg(StreamingPreferences.width).arg(StreamingPreferences.height).arg(StreamingPreferences.fps))
        var done = function(error) {
            ComputerManager.quitAppCompleted.disconnect(done)
            if (error) {
                console.warn("omnuv: play: the desktop did not quit: " + error)
                activeTarget = null
                message.show(qsTr("The desktop could not be restarted at the new resolution: %1").arg(error))
                return
            }
            beginStream(target, appIndex, want)
        }
        ComputerManager.quitAppCompleted.connect(done)
        launchApps.quitRunningApp()
    }
    function beginStream(target, appIndex, want) {
        if (!validTarget(target)) return
        // A rate that does not divide this display's refresh, chosen anywhere
        // (Advanced included), is paced rather than dropped (2 October 2026:
        // 165 on 480 Hz dropped 37% of frames). Smooth motion needs V-Sync.
        SystemProperties.refreshDisplays()
        var hz = SystemProperties.getRefreshRate(0)
        if (hz > 0 && hz % StreamingPreferences.fps !== 0 && StreamingPreferences.enableVsync
                && !StreamingPreferences.framePacing) {
            console.info("omnuv: play: " + StreamingPreferences.fps + " fps on a " + hz + " Hz display; turning Smooth motion on")
            StreamingPreferences.framePacing = true
        }
        Omnuv.setLaunchedShape(target.id, want)
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

    // **A web recipe's page, in the browser** (the operator, 2 October 2026:
    // for Ollama + Open WebUI, Play opens the web UI). On the machine's
    // private name, so only over this device's network: without it the name
    // does not resolve, and the page would fail somewhere a person cannot see
    // why. Nothing is published on the internet for it.
    function openWeb(row) {
        var url = "http://" + Omnuv.machines.hostAt(row) + ":" + Omnuv.machines.webPortAt(row) + "/"
        // The same check `omnuv://open` makes: on this project's network, not
        // merely on some network.
        if (!Omnuv.onProjectNetwork()) {
            console.info("omnuv: open: " + url + " needs this device on the network first")
            message.show(qsTr("This device is not on %1's private network. Use Join this device above, then Open again.").arg(Omnuv.projectName))
            return
        }
        console.info("omnuv: open: " + url)
        if (!Qt.openUrlExternally(url))
            message.show(qsTr("No browser could be opened. Open %1 yourself.").arg(url))
    }

    function openTerminalFor(row) {
        var host = Omnuv.machines.hostAt(row)
        var user = Omnuv.machines.userAt(row)
        if (!Omnuv.openTerminal(host, user))
            message.show(qsTr("No terminal could be started. Connect with:\n\nssh %1@%2").arg(user).arg(host))
    }
    function connectTo(row, choose) { connectTarget(Omnuv.connectionTarget(row), choose === true) }
    // When the attempt in progress began, so a second press can tell a
    // connection still working from one left behind.
    property double busySince: 0
    // Set when a press was told an attempt is in progress; the next press
    // means "start over", as the message says.
    property bool busyNoticed: false
    // Host-adds abandoned while still in flight. Their completions still
    // arrive, in order, and must not finish a newer attempt: each is consumed
    // here instead (the slot `cancelConnection` keeps exists for this).
    property int abandonedAdds: 0
    readonly property int staleAfterMs: 20000
    function busyTarget() { return activeTarget || pendingTarget }
    // **Play never does nothing silently** (30 September 2026: a press while
    // an earlier attempt's host-add had never answered returned here without a
    // word or a log line, and every later press did the same). An attempt
    // whose machine is gone, or that has been busy past `staleAfterMs`, is
    // cleared and this press starts afresh; a younger one says what it is
    // waiting for.
    function connectTarget(target, choose) {
        if (activeTarget || pendingTarget || delivering) {
            var busy = busyTarget()
            var gone = !busy || Omnuv.targetRow(busy) < 0
            var age = Date.now() - busySince
            if (gone || age > staleAfterMs || busyNoticed) {
                console.info("omnuv: play: clearing an earlier attempt (" + (gone ? "its machine is gone" : busyNoticed ? "asked to start over" : Math.round(age / 1000) + "s old") + ")")
                cancelConnection()
                if (pendingTarget) abandonedAdds++
                pendingTarget = null
                delivering = false
            } else {
                console.info("omnuv: play: an attempt for " + (busy ? busy.host : "?") + " is " + Math.round(age / 1000) + "s old; not starting another")
                message.show(qsTr("Still connecting to %1. Wait a moment, or press Play again to start over.").arg(busy ? root.machineName(busy) : ""))
                busyNoticed = true
                return
            }
        }
        if (!validTarget(target, true)) {
            console.info("omnuv: play: the machine is no longer listed")
            message.show(qsTr("That machine is no longer listed. Refresh, then try again."))
            return
        }
        console.info("omnuv: play: connecting to " + target.host + (choose ? " (choosing what to stream)" : ""))
        busySince = Date.now()
        busyNoticed = false
        activeTarget = target
        chooseApp = choose
        var row = Omnuv.targetRow(target)
        if (!Omnuv.machines.streamedAt(row)) { openTerminalFor(row); activeTarget = null; return }
        if (Omnuv.streamHostFor(ComputerManager, target) >= 0) { openHost(target); return }
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
        function onDeployFinished(ok, text) { if (!ok) message.show(text) }
        function onDeleteFinished(ok, text) {
            if (ok) root.noteRemoving(deleteMachine.targetId, deleteMachine.targetName, deleteMachine.targetApp)
            else message.show(text)
        }
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

    // **Switching deployments while signed in (H5's window half).** The only
    // address field was on the signed-out screen, so moving to another Core
    // meant signing out first, which revoked this one's sign-in. Each Core has
    // its own sign-in slot now: this changes the address and nothing else, and
    // the other Core's sign-in, if there is one, is simply read.
    Dialog {
        id: switchDeployment
        footer: OmnuvButtonBox {}
        // **Opaque, and drawn here** (25 September 2026): with the window's
        // own background given to Mica, the style's dialog fill let the page
        // behind show through the content, on the rig and in every render
        // (journey run 41201d8e: "No machines yet" through the deploy form).
        background: Rectangle {
            color: switchDeployment.palette.base
            radius: Theme.radiusOverlay
            border.color: Theme.strokeCard
        }
        objectName: "switchDeployment"
        anchors.centerIn: parent
        width: Math.min(root.width - 80, 560)
        modal: true
        title: qsTr("Switch to another Omnuv deployment")
        standardButtons: Dialog.Ok | Dialog.Cancel
        // Production shows as the empty field it means, so nobody meets its
        // address unless they chose another.
        onAboutToShow: deploymentField.text = Omnuv.coreUrl === Omnuv.productionCoreUrl ? "" : Omnuv.coreUrl
        contentItem: ColumnLayout {
            spacing: Theme.spacing
            Label {
                Layout.fillWidth: true
                text: (Omnuv.signedIn
                       ? qsTr("You stay signed in here. If you have signed in to the other one before, " +
                              "you will be again; otherwise it asks you to. ")
                       : qsTr("Only if your organization runs its own Omnuv. ")) +
                      qsTr("Leave it empty for Omnuv itself.")
                wrapMode: Text.WordWrap
            }
            TextField {
                id: deploymentField
                objectName: "deploymentField"
                Layout.fillWidth: true
                placeholderText: Omnuv.productionCoreUrl
                Accessible.name: qsTr("Omnuv address")
            }
        }
        onAccepted: Omnuv.coreUrl = deploymentField.text
    }
    // **The deploy flow** (the Instances redesign, step 8): the web's two
    // steps, through `POST /v1/deploy` with an idempotency key.
    DeployDialog {
        id: deployMachine
        objectName: "deployMachine"
    }

    // Taking a machine away, asked once, in words that say what goes with it
    // (the Instances redesign, section 5): what it loses, line by line; on a
    // protected machine a box the owner ticks, which is what clears the
    // protection; Cancel first, so a stray Enter keeps the machine.
    Dialog {
        id: deleteMachine
        background: Rectangle {
            color: Theme.highContrast ? deleteMachine.palette.base : Theme.fillCard
            radius: 16
            border.color: Theme.strokeCard
        }
        Overlay.modal: Rectangle { color: "#8C000000" }
        Binding { target: deleteMachine.palette; property: "accent"; value: Theme.accent; when: !Theme.highContrast }
        Binding { target: deleteMachine.palette; property: "highlight"; value: Theme.accent; when: !Theme.highContrast }
        objectName: "deleteMachineDialog"
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 520)
        modal: true
        title: qsTr("Delete %1?").arg(targetName)
        property int row: -1
        property string targetName: ""
        property string targetId: ""
        // Core's `protected` (0195), as the card had it when this opened.
        property bool targetProtected: false
        property string targetApp: ""
        property var losses: []
        onAboutToShow: protectionBox.checked = false
        onOpened: cancelDelete.forceActiveFocus()
        contentItem: ColumnLayout {
            spacing: Theme.spacing
            Label {
                Layout.fillWidth: true
                text: qsTr("This removes the instance and everything on it. It cannot be undone.")
                wrapMode: Text.WordWrap
            }
            Repeater {
                model: deleteMachine.losses
                Label {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacing
                    text: "\u00B7  " + modelData
                    wrapMode: Text.WordWrap
                    opacity: 0.8
                }
            }
            CheckBox {
                id: protectionBox
                objectName: "deleteProtection"
                Layout.fillWidth: true
                visible: deleteMachine.targetProtected
                text: qsTr("%1 is protected. Remove the protection and delete it.").arg(deleteMachine.targetName)
            }
        }
        footer: OmnuvButtonBox {
            Button {
                id: cancelDelete
                objectName: "deleteCancel"
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                objectName: "deleteConfirm"
                text: qsTr("Delete %1").arg(deleteMachine.targetName)
                Accessible.name: text
                enabled: !deleteMachine.targetProtected || protectionBox.checked
                // The action, last, in the warn colour the web's danger
                // button uses: a Destructive role is put first by Mac's layout.
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                // Its own label, because Material (macOS, Linux) ignores
                // `palette.buttonText` as it ignores the palette's accent: the
                // parity sheet's macOS shot drew this action in plain ink.
                contentItem: Label {
                    text: parent.text
                    font: parent.font
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    color: Theme.highContrast ? palette.buttonText : Theme.fillCaution
                    opacity: parent.enabled ? 1 : 0.45
                }
                // No onClicked: the accept role accepts the dialog, once.
            }
        }
        onAccepted: Omnuv.deleteMachine(row, targetName, targetProtected && protectionBox.checked, targetId)
    }

    Dialog {
        id: networkMove
        footer: OmnuvButtonBox {}
        background: Rectangle {
            color: networkMove.palette.base
            radius: Theme.radiusOverlay
            border.color: Theme.strokeCard
        }
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
        footer: OmnuvButtonBox {}
        background: Rectangle {
            color: revokeEnrollment.palette.base
            radius: Theme.radiusOverlay
            border.color: Theme.strokeCard
        }
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
        footer: OmnuvButtonBox {}
        background: Rectangle {
            color: pairing.palette.base
            radius: Theme.radiusOverlay
            border.color: Theme.strokeCard
        }
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
        footer: OmnuvButtonBox {}
        background: Rectangle {
            color: message.palette.base
            radius: Theme.radiusOverlay
            border.color: Theme.strokeCard
        }
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
            text: qsTr("Use the email and password of your Omnuv account.")
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            opacity: 0.78
        }

        // **Signing in happens here** (the operator, 25 September 2026:
        // approving in a browser is a web page, not the application). The
        // password goes to Core once and is never kept; the device keeps the
        // token Core answers, as it keeps the one a browser approval gives.
        //
        // **And still no address on this screen**: Sign in goes to production
        // unless another deployment was chosen, and choosing one is the link
        // below, not a field here.
        TextField {
            id: emailField
            objectName: "emailField"
            Layout.fillWidth: true
            visible: Omnuv.userCode === ""
            enabled: !Omnuv.busy
            placeholderText: qsTr("Email")
            Accessible.name: qsTr("Email")
            inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
            onAccepted: passwordField.forceActiveFocus()
        }

        TextField {
            id: passwordField
            objectName: "passwordField"
            Layout.fillWidth: true
            visible: Omnuv.userCode === ""
            enabled: !Omnuv.busy
            placeholderText: qsTr("Password")
            Accessible.name: qsTr("Password")
            echoMode: TextInput.Password
            onAccepted: signInButton.clicked()
        }

        Button {
            id: signInButton
            objectName: "signInButton"
            Layout.alignment: Qt.AlignHCenter
            visible: Omnuv.userCode === ""
            enabled: !Omnuv.busy && emailField.text.trim() !== "" && passwordField.text !== ""
            text: Omnuv.busy ? qsTr("Signing in…") : qsTr("Sign in")
            Accessible.name: qsTr("Sign in")
            highlighted: true
            implicitWidth: Math.max(140, implicitContentWidth + leftPadding + rightPadding)
            onClicked: {
                Omnuv.signInWithPassword(emailField.text, passwordField.text)
                // Not held by the window once it has been sent.
                passwordField.text = ""
            }
        }

        // The browser exchange stays, for whoever would rather not type a
        // password into an application: the page opens with the code in it.
        Button {
            objectName: "signInWithBrowser"
            Layout.alignment: Qt.AlignHCenter
            visible: Omnuv.userCode === "" && !Omnuv.busy
            flat: true
            text: qsTr("Sign in with your browser instead")
            font.pixelSize: Theme.captionSize
            onClicked: Omnuv.signIn()
        }

        Button {
            objectName: "anotherDeployment"
            Layout.alignment: Qt.AlignHCenter
            visible: Omnuv.userCode === "" && !Omnuv.busy
            flat: true
            text: qsTr("Use another Omnuv deployment…")
            font.pixelSize: Theme.captionSize
            onClicked: switchDeployment.open()
        }

        // Waiting for the browser. The code is the whole interface here.
        //
        // **The window opens the page itself** (25 September 2026), with the
        // code already in it: the console's /connect takes `?code=`. Nobody
        // is shown an address to copy unless the browser could not be opened;
        // the code stays, because comparing it by eye is what tells a person
        // the page is approving this device and not somebody else's.
        ColumnLayout {
            id: waiting
            Layout.fillWidth: true
            spacing: 12
            visible: Omnuv.userCode !== ""

            property string openedFor: ""
            property bool browserFailed: false
            readonly property string page: Omnuv.signInPage
            function openPage() {
                openedFor = Omnuv.userCode
                browserFailed = !Qt.openUrlExternally(page)
            }
            Connections {
                target: Omnuv
                function onPendingChanged() {
                    if (Omnuv.userCode !== "" && waiting.page !== "" && waiting.openedFor !== Omnuv.userCode)
                        waiting.openPage()
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Approve this sign-in in your browser. The page shows this code:")
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }

            Label {
                Layout.fillWidth: true
                visible: waiting.browserFailed
                text: qsTr("The browser did not open. Go to %1 and enter the code.").arg(Omnuv.verificationUri)
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
                objectName: "openSignInPage"
                Layout.alignment: Qt.AlignHCenter
                highlighted: true
                text: qsTr("Open the sign-in page again")
                Accessible.name: text
                onClicked: waiting.openPage()
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

    // **The web's accent on the style's own controls** (section 4, Tokens):
    // a filled button, a checked chip, a focus ring are drawn by the style
    // from the palette's accent, which was the system's blue. Not under high
    // contrast, where the palette's own pair is the only promise kept, and
    // where Theme.accent itself reads the palette.
    Binding { target: root.palette; property: "accent"; value: Theme.accent; when: !Theme.highContrast }
    Binding { target: root.palette; property: "highlight"; value: Theme.accent; when: !Theme.highContrast }

    // **The web's canvas** (the Instances redesign, section 4, Tokens), under
    // everything; under high contrast it is the palette's window.
    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
        visible: Omnuv.signedIn
    }

    // Light behind the top of the window, signed out only: signed in, the
    // page is the web's, on its canvas.
    Aurora {
        visible: !Omnuv.signedIn && !Theme.highContrast
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
            onSwitchDeploymentRequested: switchDeployment.open()
            onDeployRequested: deployMachine.open()
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
        // read, so the window says what to do rather than showing a grid that
        // would wait for ever.
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

        // **The Instances page, the web's 1:1** (the Instances redesign, 3
        // October 2026; docs/plans/recipes-in-instances.md section 4): a
        // header with Deploy, the filter chips, two cards a row from 900 px,
        // the content capped at 1152 px. The summary column is gone; the
        // network join stays the notice above.
        ColumnLayout {
            id: page
            visible: !Omnuv.noProject
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.maximumWidth: 1152
            Layout.alignment: Qt.AlignHCenter
            spacing: Theme.spacingLoose

            // Read for what it decides, not drawn: what waits for capacity,
            // and which reads could not be refreshed.
            EstateHeader {
                id: estateHeader
                visible: false
                now: machineList.now
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.padding

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: qsTr("Instances")
                        font.family: Theme.displayFamily
                        font.pixelSize: 28
                        font.weight: Theme.strongWeight
                        Accessible.role: Accessible.Heading
                    }
                    Label {
                        objectName: "instancesSubtitle"
                        Layout.fillWidth: true
                        text: {
                            var n = Omnuv.machines.count
                            var parts = [Omnuv.projectName, n === 1 ? qsTr("1 instance") : qsTr("%1 instances").arg(n)]
                            if (root.estimate > 0) parts.push(qsTr("€%1/h estimated").arg(root.estimate.toFixed(2)))
                            return parts.filter(function (p) { return p !== "" }).join(" · ")
                        }
                        font.pixelSize: 14
                        opacity: 0.7
                        elide: Label.ElideRight
                    }
                }
                Button {
                    objectName: "deployButton"
                    Layout.alignment: Qt.AlignVCenter
                    highlighted: true
                    text: "+  " + qsTr("Deploy")
                    Accessible.name: qsTr("Deploy")
                    enabled: !Omnuv.ordering
                    onClicked: deployMachine.open()
                }
            }

            // The one-time console password, shown once, naming its instance.
            Rectangle {
                objectName: "passwordBanner"
                Layout.fillWidth: true
                visible: root.oneTimePassword !== ""
                implicitHeight: passwordRow.implicitHeight + 24
                radius: 12
                color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.10)
                border.color: Theme.accent
                RowLayout {
                    id: passwordRow
                    x: 16
                    y: 12
                    width: parent.width - 32
                    spacing: Theme.spacing
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("%1's console password. Shown once.").arg(root.oneTimeFor)
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        text: root.oneTimePassword
                        font.family: Theme.monoFamily
                    }
                    Button { text: qsTr("Copy"); onClicked: Omnuv.copyText(root.oneTimePassword) }
                    Button { text: qsTr("Done"); onClicked: { root.oneTimePassword = ""; root.oneTimeFor = "" } }
                }
            }

            // The filters: Core's words, each with its count; a chip with
            // none is hidden except All.
            Flow {
                objectName: "filterChips"
                Layout.fillWidth: true
                spacing: Theme.spacing
                visible: Omnuv.machines.count > 0
                Repeater {
                    model: [ { label: qsTr("All"), word: "" },
                             { label: qsTr("Running"), word: "Running" },
                             { label: qsTr("Installing"), word: "Installing" },
                             { label: qsTr("Needs attention"), word: "Needs attention" },
                             { label: qsTr("Stopped"), word: "Stopped" } ]
                    Button {
                        required property var modelData
                        readonly property int n: modelData.word === "" ? Omnuv.machines.count : root.countOf(modelData.word)
                        visible: modelData.word === "" || n > 0
                        checkable: true
                        checked: root.filter === modelData.word
                        flat: !checked
                        text: modelData.label + "  " + n
                        Accessible.name: qsTr("%1, %2").arg(modelData.label).arg(n)
                        onClicked: { root.filter = modelData.word; root.refilter() }
                    }
                }
            }

            GridView {
                id: machineList
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: shown
                clip: true
                focus: true
                keyNavigationWraps: true
                boundsBehavior: Flickable.StopAtBounds

                // Two cards a row from 900 px of content, one below; every
                // cell one height, so a row reads as a row.
                readonly property int gap: 20
                readonly property int columns: width >= 900 ? 2 : 1
                cellWidth: Math.floor(width / columns)
                cellHeight: 376

                ScrollBar.vertical: ScrollBar {}

                // Seconds are shown, so seconds have to pass: one timer for
                // the screen, and it stops when nobody is looking.
                property double now: Date.now()
                Timer {
                    interval: 1000
                    repeat: true
                    running: machineList.visible && machineList.Window.active
                    onTriggered: machineList.now = Date.now()
                }

                // Nothing rented yet: said once the list has been read, with
                // the way to rent one.
                EmptyState {
                    visible: Omnuv.machines.loaded && Omnuv.machines.count === 0 && estateHeader.waiting.length === 0
                    x: (machineList.width - width) / 2
                    y: Math.max(0, (machineList.height - height) / 2 - 24)
                    icon: Theme.icon.cloud
                    title: qsTr("No instances in %1 yet").arg(Omnuv.projectName)
                    text: qsTr("Deploy an app or a plain machine. The marketplace chooses the provider.")
                    actionText: qsTr("Deploy")
                    onAction: deployMachine.open()
                }
            }

            // Recently removed, below the grid: what was asked to go, and
            // then that it went.
            ColumnLayout {
                objectName: "recentlyRemoved"
                Layout.fillWidth: true
                visible: root.removed.length > 0
                spacing: 2
                Label {
                    text: qsTr("Recently removed")
                    font.pixelSize: 12
                    font.weight: Theme.strongWeight
                    color: Theme.muted
                }
                Repeater {
                    model: root.removed
                    Label {
                        required property var modelData
                        Layout.fillWidth: true
                        text: modelData.gone ? qsTr("%1 was removed").arg(modelData.name)
                              : modelData.app !== "" ? qsTr("Removing %1 (%2)").arg(modelData.name).arg(modelData.app)
                              : qsTr("Removing %1").arg(modelData.name)
                        font.pixelSize: 13
                        color: Theme.muted
                        elide: Label.ElideRight
                    }
                }
            }

            // The machines, filtered by Core's word. The row a card acts on
            // is its place in the machine list, which the filter does not change.
            DelegateModel {
                id: shown
                model: Omnuv.machines
                groups: [ DelegateModelGroup { id: matching; name: "matching"; includeByDefault: true } ]
                filterOnGroup: "matching"

                delegate: MachineCard {
                    id: cardDelegate
                    readonly property int row: DelegateModel.itemsIndex
                    width: machineList.cellWidth
                    height: machineList.cellHeight
                    gap: machineList.gap
                    now: machineList.now

                    onPrimaryActivated: Omnuv.machines.webPortAt(row) > 0 ? root.openWeb(row) : root.connectTo(row)
                    onTerminalRequested: root.openTerminalFor(row)
                    onChooseAppRequested: root.connectTo(row, true)
                    onSettingsRequested: streamSettings.open()
                    onPowerRequested: function (action) { Omnuv.power(Omnuv.machines.idAt(row), action) }
                    onConsoleRequested: {
                        var link = Omnuv.consoleLinkFor(Omnuv.machines.idAt(row))
                        if (link !== "" && !Qt.openUrlExternally(link))
                            message.show(qsTr("No browser could be opened. Open %1 yourself.").arg(link))
                    }
                    onDeleteRequested: {
                        deleteMachine.row = row
                        deleteMachine.targetId = Omnuv.machines.idAt(row)
                        deleteMachine.targetName = model.name
                        deleteMachine.targetProtected = deletionProtected
                        deleteMachine.losses = root.lossesOf(model)
                        deleteMachine.targetApp = model.hasApp ? model.appName : ""
                        deleteMachine.open()
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
