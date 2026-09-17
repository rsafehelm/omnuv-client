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

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2

import ComputerModel 1.0
import ComputerManager 1.0

import Omnuv 1.0
import "estate.js" as Estate

Item {
    id: root
    focus: true

    // The toolbar shows this, so it should say where a person actually is.
    objectName: Omnuv.signedIn ? qsTr("Machines") : qsTr("Sign in to Omnuv")

    // The machine whose Connect was pressed while its host was still being
    // added. -1 when nothing is waiting.
    property int pendingRow: -1

    // Set by *Choose what to stream* just before the same `connectTo` the Play
    // button calls, and consumed by `openHost`. One slot rather than a
    // parameter threaded through `connectTo`, `openHost`, `settle` and
    // `onComputerAddCompleted`, and safe for the same reason `pendingRow` is
    // one slot: there is at most one connect in flight.
    property bool chooseApp: false

    StackView.onActivated: {
        Omnuv.refresh(true)
        Omnuv.tunnel.watch(true)
    }
    StackView.onDeactivating: Omnuv.tunnel.watch(false)

    // Upstream's own list of hosts. We do not keep our own: whether a host is
    // reachable and whether it is paired are things it already knows.
    property ComputerModel hosts: createHosts()

    function createHosts() {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', root, '')
        model.initialize(ComputerManager)
        // **This line was missing, and its absence was a live defect.**
        // `PcView.createModel()` connects it (`app/gui/PcView.qml:80`) and we
        // did not, so an Omnuv pairing that failed said nothing at all and one
        // that succeeded left its dialog on screen for ever. Everything below
        // about delivering a PIN would have been a better-dressed version of
        // the same bug without it.
        model.pairingCompleted.connect(pairingFinished)
        return model
    }

    // Upstream's `pairingComplete`, with the one thing this application can do
    // that upstream cannot: carry straight on. `error` is `undefined` on
    // success — `ComputerModel::handlePairingCompleted` turns an empty string
    // into an empty QVariant (`app/gui/computermodel.cpp:225`).
    function pairingFinished(error) {
        // `ComputerManager` has one pairing signal for every host, so take the
        // attempt this view started and put it down in the same breath. A
        // second completion that is nobody's — the command line's, say — must
        // not re-enter a machine this one finished with.
        var row = pairing.row
        var index = pairing.hostIndex
        pairing.row = -1
        pairing.hostIndex = -1
        delivering = false
        if (row < 0) {
            return
        }

        pairing.close()

        if (error !== undefined) {
            message.show(error)
            return
        }

        // The model's `paired` role is read from `NvComputer::pairState`, and
        // nothing refreshes it when pairing completes — it catches up on the
        // next poll, seconds later. Re-entering `openHost` before then would
        // see an unpaired host and mint a second PIN, so tell it what we
        // already know. One shot: `openHost` clears this as it reads it, so a
        // host that is genuinely unpaired later is paired again properly.
        justPaired = index
        openHost(row)
    }

    // The host index whose pairing has just completed, for exactly one call to
    // `openHost`. -1 the rest of the time.
    property int justPaired: -1

    // True from the moment a code is minted until that attempt is over, one
    // way or the other.
    //
    // **The happy path shows nothing, which is an invitation to press Play
    // again.** A second press would mint a second code — which the machine
    // refuses, because a pairing for this client is already under way — and
    // would ask Core a second time for a login it can only issue once, so the
    // second attempt would report "already collected" over the top of a first
    // one that was working. Nothing about that is recoverable by the person,
    // and all of it is avoided by not starting twice.
    property bool delivering: false

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
            // **The third state, and it is not a shade of offline.**
            // `ComputerModel` has carried `StatusUnknownRole` all along and we
            // were dropping it, which made "I could not reach this to ask"
            // render exactly like "I asked and it said no". `docs/client-widget.md`
            // forbids that in its own words: a check that could not run is
            // `unknown`, not `fail`. Telling a buyer their machine is down when
            // the truth is that we cannot see it sends them to fix the wrong
            // thing.
            readonly property bool hostStatusUnknown: model.statusUnknown
        }
    }

    // The applications one machine publishes, while a launch is in flight.
    //
    // `AppModel` is upstream's, initialised exactly as `AppView.qml:64-69`
    // does it, and it is the only thing in this program that can build a
    // `Session`: `app/main.cpp:949` registers `Session` as uncreatable from
    // QML, and `AppModel::createSessionForApp()` is the one factory
    // (`app/gui/appmodel.cpp:97`). So this is not a convenience — it is the
    // supported way in.
    //
    // `showHiddenGames` is true, unlike `AppView`'s default, because a hidden
    // application is still the one this machine is sold as streaming. Hiding
    // is a preference about a grid we no longer show, and letting it decide
    // whether Play works would be a setting silently breaking a product
    // promise. The same value goes to `initialize()` and is therefore the same
    // index space `createSessionForApp()` reads.
    property var launchApps: null

    Repeater {
        id: appProbe
        delegate: Item {
            visible: false
            readonly property string appName: model.name
        }
    }

    // Upstream matches an application by name lowercased — `CliStartStream`'s
    // `getAppIndex()`, `app/cli/startstream.cpp:146`. Same rule here, so a
    // machine reachable from the shell is reachable from the card.
    function appIndexFor(name) {
        var want = name.toLowerCase()
        for (var i = 0; i < appProbe.count; i++) {
            var item = appProbe.itemAt(i)
            if (item && item.appName.toLowerCase() === want) {
                return i
            }
        }
        return -1
    }

    // Found by the exact address we gave it.
    //
    // Two earlier keys were wrong in different ways. Matching on the host's
    // *name* fails in the only window that matters — right after
    // `addNewHostManually` the entry exists but has not been polled, so it has
    // no name yet. Matching on the model's `details` string fails worse:
    // `ComputerModel::data` builds that from `tr("Online")`, `tr("Paired")` and
    // friends, so it is localised prose, and a substring search in it matches
    // 10.200.1.5 against a host at 10.200.1.50.
    //
    // `Omnuv.hostRowFor` compares `NvComputer::manualAddress` — structured,
    // exact, persisted, and never translated. See its comment for why the row
    // it returns is the same row this view indexes.
    function hostIndexFor(host) {
        return Omnuv.hostRowFor(ComputerManager, host)
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
                // **Upstream tells us why, and we used to throw it away.**
                // `detectedPortBlocking` means this device's own network is
                // blocking the streaming ports — a problem with the cafe wifi,
                // not with the machine. Reporting that as "your machine has not
                // finished starting" sends a person to stare at a console that
                // is working perfectly.
                if (detectedPortBlocking) {
                    message.show(qsTr("This network is blocking the ports streaming needs.\n\n" +
                                      "%1 is reachable, but this device cannot open a stream to it " +
                                      "from here. A different network, or a phone hotspot, will.")
                                 .arg(Omnuv.machines.nameAt(row)))
                    return
                }

                // Otherwise there are two causes and this device cannot tell
                // them apart: it is not on the Client VPN, or the machine is up
                // but nothing is streaming on it. Say both rather than guess.
                message.show(qsTr("No streaming host answered at %1.\n\n" +
                                  "Either this device is not on your Client VPN, or the machine " +
                                  "has not finished setting itself up. Its console says which.")
                             .arg(Omnuv.machines.hostAt(row)))
                return
            }
            root.openHost(row)
        }
    }

    // Both the entry and its state lag the signal that created it: the host is
    // added, then polled, and only then does it have a name or count as online.
    // Twice now that lag has looked like a broken machine — first the name was
    // missing, then `online` was false on a host that had just answered. So
    // wait for it rather than judging it on the first look.
    Timer {
        id: settle
        property int row: -1
        property int tries: 0
        interval: 500
        repeat: true
        onTriggered: {
            var index = root.hostIndexFor(Omnuv.machines.hostAt(row))
            var item = index >= 0 ? hostProbe.itemAt(index) : null
            if (item && item.hostOnline) {
                stop()
                root.openHost(row)
            }
            else if (++tries > 30) {
                stop()
                // Three answers, not two. "We asked and it said no" and "we
                // could not ask" send a person to different places, so they get
                // different sentences.
                if (item && item.hostStatusUnknown) {
                    message.show(qsTr("This device cannot tell whether %1 is up.\n\n" +
                                      "That is usually the Client VPN rather than the machine: " +
                                      "nothing answered, so there is nothing to report about it " +
                                      "either way.")
                                 .arg(Omnuv.machines.nameAt(row)))
                }
                else {
                    message.show(qsTr("%1 is not answering yet. A machine takes a few minutes to " +
                                      "finish setting itself up after it starts.")
                                 .arg(Omnuv.machines.nameAt(row)))
                }
            }
        }
        function watch(r) {
            row = r
            tries = 0
            start()
        }
    }

    // Everything Connect does, once the host is known to be there and awake.
    function openHost(row) {
        var index = hostIndexFor(Omnuv.machines.hostAt(row))
        var item = index >= 0 ? hostProbe.itemAt(index) : null
        if (!item || !item.hostOnline) {
            settle.watch(row)
            return
        }

        var alreadyPaired = (index === justPaired)
        justPaired = -1

        if (!item.hostPaired && !alreadyPaired) {
            if (delivering) {
                // A code is already out on this machine and it is waiting for
                // that one. Show it again rather than minting a second, which
                // the machine refuses — Sunshine answers 409 to a second
                // pairing from the same client — and which would ask Core for
                // a login it can only ever issue once.
                if (pairing.why !== "") {
                    pairing.open()
                }
                return
            }

            // **The PIN is made here and handed over for them.**
            //
            // It used to say that this could not be done for a person, because
            // the machine has never seen the number. That was true of a machine
            // nobody vouches for; it is not true of one the marketplace built.
            // The machine mints a single-use login for its own streaming host,
            // Core holds it for its owner, and this device spends it — so the
            // number still exists and nobody reads it off a screen.
            //
            // The order is the machine's rather than ours: `pairComputer` has
            // to go first, because the identifier a PIN is addressed to does
            // not exist until this client's pairing request is waiting on the
            // machine. See `app/omnuv/pairing.cpp`.
            var pin = hosts.generatePinString()
            hosts.pairComputer(index, pin)

            pairing.machine = Omnuv.machines.nameAt(row)
            pairing.host = Omnuv.machines.hostAt(row)
            pairing.pin = pin
            pairing.row = row
            pairing.hostIndex = index
            pairing.why = ""
            pairing.detail = ""

            // Opens nothing on the happy path. `Omnuv.pairingFailed` is what
            // brings the dialog up, which is what makes it a fallback rather
            // than a step.
            delivering = true
            Omnuv.deliverPin(row, pin)
            return
        }

        if (root.chooseApp) {
            root.chooseApp = false
            openAppGrid(row)
            return
        }
        launch(row)
    }

    // Upstream's grid, which used to be where Play landed and is now where
    // *Choose what to stream* lands. Unchanged, in upstream's style, inside
    // upstream's chrome: it is upstream's screen and restyling it would be a
    // sixth file in the change budget for a screen a person opens once.
    function openAppGrid(row) {
        var index = hostIndexFor(Omnuv.machines.hostAt(row))
        var component = Qt.createComponent("qrc:/gui/AppView.qml")
        var appView = component.createObject(stackView, {
                                                 "computerIndex": index,
                                                 "objectName": Omnuv.machines.nameAt(row)
                                             })
        stackView.push(appView)
    }

    // Ask the machine what it publishes, then stream the one Core named.
    //
    // The list is not there when the host first comes online — upstream polls
    // serverinfo and the application list separately, which is why
    // `CliStartStream` sits in `StateSeekApp` waiting for an
    // `Event::ComputerUpdated` (`app/cli/startstream.cpp:96-112`) rather than
    // reading it straight away. So this waits the same way `settle` waits for
    // the host, and for the same reason.
    function launch(row) {
        var index = hostIndexFor(Omnuv.machines.hostAt(row))
        if (index < 0) {
            // Cannot happen from `openHost`, which has already found it. If it
            // ever does, the grid is the honest fallback rather than a guess.
            openAppGrid(row)
            return
        }

        if (launchApps) {
            // Dropped from the probe before it is destroyed, because
            // `destroy()` is deferred to the end of the event loop and a
            // Repeater holding a model that is about to go is a crash looking
            // for a busy machine.
            appProbe.model = null
            launchApps.destroy()
        }
        launchApps = Qt.createQmlObject('import AppModel 1.0; AppModel {}', root, '')
        launchApps.initialize(ComputerManager, index, true)
        appProbe.model = launchApps

        appWait.row = row
        appWait.tries = 0
        appWait.start()
    }

    // The list, once there is one.
    Timer {
        id: appWait
        property int row: -1
        property int tries: 0
        interval: 500
        repeat: true
        // The usual case is a machine this device has streamed before, whose
        // list upstream already has. Waiting half a second to discover that
        // would be half a second of nothing on every Play.
        triggeredOnStart: true
        onTriggered: {
            if (row < 0) {
                stop()
                return
            }

            if (appProbe.count > 0) {
                stop()
                var want = Omnuv.machines.streamAppAt(row)
                var app = root.appIndexFor(want)
                var r = row
                row = -1
                if (app >= 0) {
                    root.startStream(r, app)
                }
                else {
                    // The machine answered and this is not on its list. That is
                    // a real disagreement between Core and the machine, and the
                    // person is better served by the grid — which shows what is
                    // actually there — than by a sentence telling them so.
                    message.show(qsTr("%1 is not offering \u201C%2\u201D right now. Choose what to " +
                                      "stream from what it does offer.")
                                 .arg(Omnuv.machines.nameAt(r)).arg(want))
                    root.openAppGrid(r)
                }
                return
            }

            if (++tries > 30) {
                stop()
                var name = Omnuv.machines.nameAt(row)
                row = -1
                message.show(qsTr("%1 answered, but has not said what it can stream. Its streaming " +
                                  "host is still starting up, or it stopped after answering.")
                             .arg(name))
            }
        }
    }

    // Build the session and hand it to our own segue.
    //
    // `createSessionForApp` is the only QML-reachable `Session` constructor —
    // see `launchApps` above — and the object it returns has JavaScript
    // ownership, so the segue's `property Session session` is what keeps it
    // alive. Upstream relies on exactly that in `AppView.qml:221-227`.
    function startStream(row, appIndex) {
        var component = Qt.createComponent("qrc:/omnuv/OmnuvSegue.qml")
        var segue = component.createObject(stackView, {
                                               "session": launchApps.createSessionForApp(appIndex),
                                               "appName": Omnuv.machines.streamAppAt(row),
                                               "machineName": Omnuv.machines.nameAt(row),
                                               "machineHost": Omnuv.machines.hostAt(row),
                                               "machineUser": Omnuv.machines.userAt(row)
                                           })

        // A `Session` runs once — `DeferredSessionCleanupTask`'s destructor
        // releases `s_ActiveSessionSemaphore` — so *Try again* cannot restart
        // the one that failed. It comes back here and starts over, which is
        // also the only place that knows how to.
        segue.retryRequested.connect(function() {
            stackView.pop()
            root.connectTo(row)
        })

        stackView.push(segue)
    }

    // The primary action of a machine that does not stream, and the *other*
    // way in to one that does. Split out of connectTo() because a streamed
    // machine's card offers both, and a rig that will not stream is exactly
    // when a person needs the terminal most.
    function openTerminalFor(row) {
        var host = Omnuv.machines.hostAt(row)
        var user = Omnuv.machines.userAt(row)
        if (!Omnuv.openTerminal(host, user)) {
            message.show(qsTr("No terminal could be started. Connect with:\n\nssh %1@%2")
                         .arg(user).arg(host))
        }
    }

    function connectTo(row) {
        if (!Omnuv.machines.streamedAt(row)) {
            openTerminalFor(row)
            return
        }

        if (hostIndexFor(Omnuv.machines.hostAt(row)) >= 0) {
            openHost(row)
            return
        }

        // First time on this device: teach the streaming client the machine's
        // private name. `addNewHostManually` is the one upstream method this
        // application calls, and the whole integration rests on it.
        pendingRow = row
        ComputerManager.addNewHostManually(Omnuv.machines.hostAt(row))
    }

    // The only ending of a delivery that has a screen. Success is silent: the
    // machine takes the code, upstream finishes the handshake, and
    // `pairingFinished` carries on to the stream.
    Connections {
        target: Omnuv

        function onPairingFailed(why, detail) {
            // The attempt is *not* over: only its automatic half failed, and
            // the machine is still waiting for this code because
            // `pairComputer` is still running. So `delivering` stays true and
            // `pairing.row` stays set until the pairing itself ends, one way
            // or the other.
            pairing.why = why
            pairing.detail = detail
            pairing.open()
        }
    }

    // ---------------------------------------------------------------- dialogs

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
        property int row: -1
        property int hostIndex: -1

        // One sentence for the person, and the machine-shaped remainder.
        property string why
        property string detail

        anchors.centerIn: parent
        width: Math.min(root.width - 80, 460)
        modal: true
        standardButtons: Dialog.Close
        title: qsTr("Finish pairing with %1").arg(machine)

        onClosed: disclosure.shown = false

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
                text: qsTr("It is waiting for this number now, on its own setup page, and stops " +
                           "waiting five minutes after Play was pressed.")
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.bodySize
                opacity: 0.7
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: pairing.pin
                font.pixelSize: Theme.titleSize
                font.letterSpacing: 6
                font.family: "monospace"
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
                font.family: "monospace"
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

        Rectangle {
            width: 3
            height: parent.height
            radius: 1.5
            color: notice.tone
        }

        RowLayout {
            id: noticeRow
            anchors.fill: parent
            anchors.leftMargin: Theme.padding
            anchors.rightMargin: Theme.spacing
            spacing: Theme.spacingLoose

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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.padding + Theme.spacing
        spacing: Theme.spacingLoose
        visible: Omnuv.signedIn

        OrganizationBand {
            Layout.fillWidth: true
        }

        // Whether this device is on the project network at all. Shown before
        // the machines, because "Connect does nothing" is nearly always this.
        Notice {
            visible: !Omnuv.tunnel.connected
            text: Omnuv.tunnel.state
            actionText: Omnuv.tunnel.busy ? qsTr("Joining…") : qsTr("Join this device")
            actionEnabled: Omnuv.tunnel.available && !Omnuv.tunnel.busy && Omnuv.signedIn
            onAction: Omnuv.tunnel.join()
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

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width, 460)
                spacing: Theme.spacing

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("No project yet")
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.subtitleSize
                    font.weight: Theme.strongWeight
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("This organization has no project to show. Signing in to the Omnuv console sets up a new workspace, and this window fills in once it exists.")
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                    opacity: 0.7
                }
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: Theme.spacing
                    text: qsTr("Check again")
                    onClicked: Omnuv.refresh(true)
                }
            }
        }

        RowLayout {
            id: estateBody
            visible: !Omnuv.noProject
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.padding + Theme.spacing

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
                    cellHeight: 224

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

                    delegate: MachineCard {
                        width: machineList.cellWidth
                        height: machineList.cellHeight
                        gap: machineList.gap
                        now: machineList.now

                        onPrimaryActivated: root.connectTo(index)
                        onTerminalRequested: root.openTerminalFor(index)
                        onChooseAppRequested: {
                            root.chooseApp = true
                            root.connectTo(index)
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
                                            ctx.roundedRect(0, 0, width, height, Theme.radiusControl, Theme.radiusControl)
                                            ctx.stroke()
                                        }
                                    }

                                    ColumnLayout {
                                        x: Theme.padding
                                        y: Theme.padding
                                        width: outline.width - 2 * Theme.padding
                                        height: outline.height - 2 * Theme.padding
                                        spacing: Theme.spacing

                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Waiting for capacity")
                                            font.family: Theme.textFamily
                                            font.pixelSize: Theme.bodySize
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

            // The rail, with a hairline between it and the machines rather
            // than a box around it. It scrolls on its own when it is taller
            // than the window.
            Rectangle {
                visible: estateBody.wideEnough
                Layout.fillHeight: true
                implicitWidth: 1
                color: Theme.strokeCard
            }

            Flickable {
                id: railScroller
                visible: estateBody.wideEnough
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                contentHeight: sideRail.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                EstateRail {
                    id: sideRail
                    width: railScroller.width
                    now: machineList.now
                }
            }
        }

        // What Omnuv has to say, read-only, folded to one line until opened.
        MessagesPanel {
            visible: !Omnuv.noProject
            Layout.fillWidth: true
            now: machineList.now
        }
    }
}
