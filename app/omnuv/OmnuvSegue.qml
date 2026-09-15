// Omnuv: what a person watches between pressing Play and seeing the machine.
//
// Upstream's `app/gui/StreamSegue.qml` does the same job in two controls — a
// spinner and one label — and reports a failure as
// `Starting %1 failed: Error %2`, where `%1` is moonlight-common-c's internal
// stage name and `%2` is an integer. A buyer is never shown either. This file
// is that screen with the words replaced: a ladder that says which of five
// things is happening, and a failure that is a sentence, the actions that
// apply to it, and the numbers folded away for whoever is asked to help.
//
// **A separate file rather than an edit.** `app/gui/StreamSegue.qml` is not in
// the change budget, and it should not be: `CliQuitStreamSegue` pushes it for
// the `quit` verb and expects upstream's behaviour, dialog included — and the
// budget check pins that file as the one place upstream's numeric-code dialog
// is referenced from, so editing it out would fail CI on the step that exists
// to keep that dialog away from buyers. The two share nothing but the seven
// signals on `Session`, which is the contract that actually matters.
//
// **This file is now on both paths, and that is the point.** It was reached
// only from `OmnuvView.startStream()` until 15 September 2026, when the
// Windows rig measured what `OmnuvClient.exe stream <machine> "<app>"` actually
// drew and answered `SEGUE=absent`: upstream's `CliStartStreamSegue` and its
// Material `ErrorMessageDialog`, with upstream's words. That command is what
// `omnuv-connect --stream` runs, what every `omnuv://stream` link from the
// console runs, and what the gaming-rig end-to-end streams with — so it was
// the buyer-facing surface for everyone outside the widget, and the one an
// automated proof would have called *the same*. `app/omnuv/OmnuvCliSegue.qml`
// is that verb's view now, and it is this file with a launcher attached.
//
// Every signal below is connected because upstream connects it, and the list
// is read from `app/streaming/session.h:128-144` rather than from memory.
// `launchWarningsChanged` is the eighth and is deliberately not connected:
// `launchWarnings` is read once as a property in `Loader.onLoaded`, which is
// what upstream does and when the value is actually populated.

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0
import Session 1.0
import SystemProperties 1.0

import Omnuv 1.0

Item {
    id: segue
    focus: true

    // Escape closes the failure panel, and only the failure panel. Upstream's
    // failure is a `NavigableMessageDialog`, which handles Escape for free;
    // ours is a panel on a pushed view, and a screen a person cannot leave
    // with the key every other screen leaves with is a screen they will
    // describe as frozen. Nothing while the ladder is up: there is no way to
    // interrupt a connection attempt and pretending otherwise would be worse
    // than not offering it.
    Keys.onEscapePressed: if (segue.failed) segue.leave()

    // Set by whoever pushes this. `session` comes from
    // `AppModel::createSessionForApp()`, the one factory QML has — `Session`
    // itself is registered uncreatable (`app/main.cpp:949`).
    property Session session
    property string appName
    property string machineName
    property string machineHost
    property string machineUser

    // Asked for by the failure panel's *Try again*. The push site owns what
    // that means, because starting over means building a new `Session` and
    // this one is spent: `Session::s_ActiveSessionSemaphore` is released in
    // `DeferredSessionCleanupTask`'s destructor and a `Session` runs once.
    signal retryRequested()

    // ------------------------------------------------------- the command line
    //
    // **Set only by `OmnuvCliSegue.qml`**, which is what `OmnuvClient.exe stream
    // <machine> "<app>"` opens. When it is set there is no `Session` yet: this
    // view is the whole program, and a `CliStartStream::Launcher` is still
    // looking for the machine. Null on the widget's path, where `OmnuvView`
    // has already built the session it hands over.
    //
    // A `Launcher`, kept as `var` because `CliStartStream::Launcher` is not
    // registered as a QML type — `app/main.cpp` publishes it as a root context
    // property, which is how upstream's three CLI views reach it too.
    property var cli: null

    // How far the launcher got, because `Launcher::failed(QString)` says what
    // went wrong only in prose (`app/cli/startstream.h:34`). 1 while it is
    // looking for the machine, 2 while it is asking what the machine
    // publishes, 3 while it is stopping something already running. Read
    // instead of the message, because matching translated English is a check
    // that stops matching in the first localised build.
    property int cliStep: 0

    // True when there is nothing behind this view, so leaving it is leaving
    // the program. Upstream's `StreamSegue` calls the same thing `quitAfter`
    // (`app/gui/StreamSegue.qml:15`). It is not optional on the command line:
    // Omnuv sets `setQuitOnLastWindowClosed(false)` for the tray, so a CLI run
    // that nobody quits explicitly never exits.
    property bool quitOnLeave: false

    // The one door out of this screen, so Escape and *Close* cannot disagree.
    function leave() {
        if (quitOnLeave) {
            Qt.quit()
        }
        else {
            stackView.pop()
        }
    }

    // ---------------------------------------------------------------- stages
    //
    // **The five rungs, and the eleven stages that land on them.**
    //
    // The stage strings are moonlight-common-c's, verbatim, from
    // `moonlight-common-c/src/Connection.c:39-53` — `stageNames[]`, returned by
    // `LiGetStageName()` and handed to us by `Session::clStageStarting`
    // (`app/streaming/session.cpp:74`). `Limelight.h:372` calls them "stable
    // stage names", which is the only reason it is safe to switch on them.
    // They are not translated: `QString::fromLocal8Bit` of a C string literal.
    //
    // The order is fixed by `LiStartConnection` and the ladder must not walk
    // backwards, so the rungs are contiguous runs of that order rather than a
    // tidy grouping by subject. See the report for the one fold this makes
    // uncomfortable — "audio stream initialization" sits under *Agreeing the
    // stream* because it runs immediately before the RTSP handshake.
    readonly property var ladder: [
        qsTr("Preparing this device"),
        qsTr("Finding %1 on your private network").arg(machineName),
        qsTr("Agreeing the stream with %1").arg(machineName),
        qsTr("Setting up video, sound and your controls"),
        qsTr("Waiting for the first frame")
    ]

    // Which rung is lit. -1 once the picture is up and this screen is gone.
    property int rung: 0

    // Whether there is anything to show yet. A state rather than an assignment
    // to `ladderPanel.visible`, because the ladder has to disappear for two
    // unrelated reasons — the picture arrived, or the thing failed — and one
    // imperative write would destroy the binding the other needs.
    property bool revealed: false

    // What each stage means to a person, and what they can do about it.
    //
    // `step` is the rung it lights. `text` is the sentence shown when that
    // stage *fails* — the rung label is what they read while it works, and the
    // two are deliberately different: "Waiting for the first frame" is not a
    // useful thing to be told about a failure.
    //
    // `actions` names only the stage-specific buttons. *Try again* and *Close*
    // are on every failure and are not listed; *Join this device* appears
    // whenever the private network is down, whatever failed, because it is
    // always the next thing to try when it is.
    //
    // Each sentence says what actually failed, read from the function
    // `Connection.c` calls at that stage rather than from the stage's name.
    // Three of the names are misleading enough to matter: "video stream
    // establishment" is this device's decoder starting (`startVideoStream` ->
    // `VideoCallbacks.setup`, `VideoStream.c:325`), "audio stream
    // establishment" is this device's audio output (`AudioCallbacks.init`,
    // `AudioStream.c:442`), and neither is a network step. The only two stages
    // that are — RTSP handshake and control stream establishment — are exactly
    // the two `LiGetPortFlagsFromStage` has ports for
    // (`ConnectionTester.c:17-31`), which is the corroboration.
    function stageInfo(stage) {
        switch (stage) {
        case "none":
            // `stageNames[0]`, the initial value of `Connection.c`'s static
            // `stage`. No callback is ever invoked with it — neither
            // `stageStarting` nor `stageFailed` — so this case exists to keep
            // the table total rather than because anyone will read it. A plain
            // sentence, not a guess at a cause.
            return { step: 0, text: qsTr("Streaming to %1 could not start.").arg(machineName), actions: [] }

        case "platform initialization":
            // `initializePlatform()` — this device's sockets, threads and
            // crypto. Nothing has been asked of the machine yet.
            return { step: 0,
                     text: qsTr("Streaming could not start on this device. Nothing is wrong with %1 — " +
                                "this device could not set up its own networking for the stream.").arg(machineName),
                     actions: [] }

        case "name resolution":
            // `resolveHostName()` — `getaddrinfo`, and then a TCP connect test
            // against 47984, 47989 and 48010 (`PlatformSockets.c:671`,
            // `Connection.c:346-377`). So it covers both "the name is unknown
            // here" and "the name is known and nothing answered", and the
            // sentence has to cover both too.
            return { step: 1,
                     text: qsTr("This device could not reach %1 on your private network. Its name, %2, " +
                                "either did not resolve here or answered nothing — which is usually this " +
                                "device not being joined to the network rather than the machine being down.")
                            .arg(machineName).arg(machineHost),
                     actions: [] }

        case "audio stream initialization":
            // `initializeAudioStream()` allocates two queues and a crypto
            // context and returns 0 unconditionally (`AudioStream.c:68-85`), so
            // this failure is unreachable at the pinned submodule. Kept because
            // the call site checks for it and the next release may mean it.
            // Not a sentence about sound: nothing about the audio *output*
            // happens here.
            return { step: 2,
                     text: qsTr("Streaming could not set itself up on this device."),
                     actions: [] }

        case "RTSP handshake":
            // `performRtspHandshake()` — the one real conversation with the
            // machine before the streams start, over TCP 48010 or ENet. This
            // and the control stream are the only two stages that carry
            // failing ports.
            return { step: 2,
                     text: qsTr("%1 answered, but would not agree a stream. It is usually already " +
                                "streaming to another device, or its streaming host restarted while " +
                                "this device was connecting.").arg(machineName),
                     actions: ["terminal"] }

        case "control stream initialization":
            // `initializeControlStream()` — queues, a mutex and an event on
            // this device; returns 0 unconditionally (`ControlStream.c:301-361`),
            // so unreachable at the pinned submodule, same as audio above.
            return { step: 3,
                     text: qsTr("Streaming could not set itself up on this device."),
                     actions: [] }

        case "video stream initialization":
        case "input stream initialization":
            // `initializeVideoStream()` and `initializeInputStream()` return
            // void and have no failure call site at all (`Connection.c:467-481`).
            // They light a rung and never fail.
            return { step: 3,
                     text: qsTr("Streaming could not set itself up on this device."),
                     actions: [] }

        case "control stream establishment":
            // `startControlStream()` — an ENet connect to the machine on UDP
            // 47999. A genuine network step, and the only one after the
            // handshake.
            return { step: 4,
                     text: qsTr("The channel that carries your keyboard and mouse to %1 could not be " +
                                "opened. Something between this device and the machine is dropping it.")
                            .arg(machineName),
                     actions: ["terminal"] }

        case "video stream establishment":
            // `startVideoStream()` — this device's decoder first
            // (`VideoCallbacks.setup`), then a local UDP bind. Both are here,
            // not on the machine, which is why the settings that matter are
            // this device's.
            return { step: 4,
                     text: qsTr("The picture from %1 could not be started. This device could not begin " +
                                "decoding it — usually the chosen resolution, frame rate or codec is more " +
                                "than its graphics can decode.").arg(machineName),
                     actions: ["settings"] }

        case "audio stream establishment":
            // `startAudioStream()` — `AudioCallbacks.init`, this device's
            // audio output, then a local bind.
            return { step: 4,
                     text: qsTr("The sound from %1 could not be started. This device's audio output " +
                                "refused the format the stream uses, or is in use by something else.")
                            .arg(machineName),
                     actions: ["settings"] }

        case "input stream establishment":
            // `startInputStream()` — a send thread on this device for every
            // host newer than gen 4.
            return { step: 4,
                     text: qsTr("Your keyboard, mouse and controller could not be connected to %1. " +
                                "The picture would have worked; the controls would not.").arg(machineName),
                     actions: [] }
        }

        // An upstream release that adds a stage lands here. A plain sentence
        // and no invented cause — the stage name itself is in Details, which
        // is where somebody who can act on it will look.
        // `.github/workflows/omnuv-change-budget.yml` fails the build when a
        // name in `stageNames[]` has no case above, so this should never be
        // reached in a shipped build.
        return { step: segue.rung, text: qsTr("Streaming to %1 stopped before it started.").arg(machineName), actions: [] }
    }

    // ---------------------------------------------------------------- failure
    property bool failed: false
    property string failHeadline: ""
    property string failExtra: ""
    property string failDetails: ""
    property var failActions: []

    function addDetail(line) {
        failDetails += (failDetails === "" ? "" : "\n") + line
    }

    // **What this screen put in front of a person, on one line, for something
    // that cannot look at it.**
    //
    // `deployment/ansible/files/omnuv-run.ps1` drives a stream against a name
    // under `.invalid` and reads the result out of the log, because a
    // screenshot needs OCR to be asserted on and an empty panel photographs
    // exactly like a black screen. Its contract is two lines:
    //
    //     Omnuv segue: failed headline=<the sentence, on one line>
    //     Omnuv segue: details=<the technical half, on one line>
    //
    // Three things about the shape, each of which the harness depends on.
    //
    // **`console.info`, not `console.error`.** Qt maps the four console
    // methods onto logging levels in `writeToConsole`
    // (`qtdeclarative/src/qml/qml/qqmlbuiltinfunctions.cpp`, branch 6.10):
    // `Info -> logger.info()`, `Error -> logger.critical()`. Moonlight's own
    // handler renders those as `Qt Info:` and `Qt Critical:`
    // (`app/main.cpp:216-220`), and the harness anchors on `Qt Info:`.
    // `s_SuppressVerboseOutput` would drop it, and is set only for the `list`
    // action (`app/main.cpp:821-828`), never for `stream`.
    //
    // **Emitted where the text is rendered, not where it was decided.** These
    // are property-change handlers rather than calls at the end of
    // `stageFailed` and `cliFailed`, because both of those can be followed by
    // `displayLaunchError` and by `sessionFinished` adding to the panel. A
    // handler fires on whatever actually ended up bound to the Label.
    //
    // **Whitespace collapsed.** `Get-Marker` captures `(.*)`, which does not
    // cross a newline, and `failDetails` is deliberately multi-line. It takes
    // the *last* match, which is why `details=` may be written several times
    // as lines accumulate: the last one is the whole of it.
    //
    // This reports and does not grade. Whether what was drawn was a sentence,
    // and whether it contained a number, are judged by the harness, out of
    // this file's reach — a client that graded its own output would be a check
    // that cannot fail.
    function oneLine(text) {
        return text.replace(/\s+/g, " ").trim()
    }

    onFailHeadlineChanged: {
        if (failHeadline !== "") {
            console.info("Omnuv segue: failed headline=" + oneLine(failHeadline))
        }
    }

    onFailDetailsChanged: {
        if (failDetails !== "") {
            console.info("Omnuv segue: details=" + oneLine(failDetails))
        }
    }

    // Upstream writes one of its sentences and, in exactly one branch, a
    // number: `clConnectionTerminated`'s default case emits
    // `tr("Connection terminated") + "\n\n" + tr("Error code: %1")`
    // (`app/streaming/session.cpp:129-132`). That number is folded into
    // Details like any other.
    //
    // ponytail: folded by shape — a final paragraph that is one short line
    // carrying a digit — rather than by matching "Error code:", which is
    // translated and would stop matching in the first localised build. Ceiling:
    // a genuinely short numeric sentence would fold too; nothing upstream emits
    // one, and the fold moves text rather than losing it.
    function splitTrailingCode(text) {
        var parts = text.split("\n\n")
        var tail = parts[parts.length - 1]
        if (parts.length > 1 && tail.indexOf("\n") < 0 && tail.length <= 40 && /[0-9]/.test(tail)) {
            parts.pop()
            return { body: parts.join("\n\n"), code: tail }
        }
        return { body: text, code: "" }
    }

    // ---------------------------------------------------------------- signals

    function stageStarting(stage) {
        rung = stageInfo(stage).step
    }

    function stageFailed(stage, errorCode, failingPorts) {
        var info = stageInfo(stage)
        rung = info.step
        failed = true
        failHeadline = info.text
        failActions = info.actions

        // The stage name and the code are facts about the program, not about
        // the person's machine, so they go where facts about the program go.
        addDetail(qsTr("Stage: %1").arg(stage))
        addDetail(qsTr("Code: %1").arg(errorCode))
        if (failingPorts) {
            addDetail(qsTr("Ports: %1").arg(failingPorts))
        }
    }

    function displayLaunchError(text) {
        var split = splitTrailingCode(text)
        failed = true
        // A stage sentence, when there is one, is the headline: it says which
        // part of the chain gave way. Upstream's text is more specific about
        // why and reads second. With no stage failure it is the headline.
        if (failHeadline === "") {
            failHeadline = split.body
        }
        else if (split.body !== "" && failExtra.indexOf(split.body) < 0) {
            failExtra += (failExtra === "" ? "" : "\n\n") + split.body
        }
        if (split.code !== "") {
            addDetail(split.code)
        }
        console.error(text)
    }

    function connectionStarted() {
        // The picture is up. Nothing of ours should be behind it when the
        // window comes back for a moment during the handover.
        rung = -1
        revealed = false
        window.visible = false
    }

    function quitStarting() {
        // Upstream's screen for "closing the application on the machine".
        // Reused rather than reimplemented: it is three labels and it is only
        // reached when `quitAppAfter` is set.
        var component = Qt.createComponent("qrc:/gui/QuitSegue.qml")
        stackView.replace(stackView.currentItem,
                          component.createObject(stackView, {"appName": appName}),
                          StackView.Immediate)
        window.visible = true
    }

    function sessionFinished(portTestResult) {
        // `-1` is "the test did not run", `0` is "nothing was blocked". Any
        // other value is this device's own internet blocking the ports, which
        // is worth saying beside a failure and meaningless without one.
        if (portTestResult !== 0 && portTestResult !== -1 && failed) {
            failExtra += (failExtra === "" ? "" : "\n\n") +
                    qsTr("This device's internet connection is also blocking the ports streaming uses, " +
                         "so a different network — or a phone hotspot — may be the quickest way through.")
        }

        SdlGamepadKeyNavigation.enable()
        window.visible = true

        if (!failed) {
            // `leave()` rather than `pop()`: on the command line there is
            // nothing underneath, and upstream's segue quits here for exactly
            // that reason (`app/gui/StreamSegue.qml:76-79`).
            leave()
        }
        // On a failure this screen stays and becomes the panel — on both
        // paths, including the command line, where upstream would have quit
        // as soon as its dialog was dismissed. Upstream pops and opens
        // `streamSegueErrorDialog`, a `main.qml` id; nothing here touches it,
        // which is what "upstream's dialog is not shown from our path" means
        // in practice.
    }

    function sessionReadyForDeletion() {
        // A Session keeps SDL_TTF and friends alive. Upstream's words, and
        // upstream's reason.
        session = null
        gc()
    }

    StackView.onDeactivating: {
        toolBar.visible = true
        SdlGamepadKeyNavigation.enable()
    }

    // ---------------------------------------------- the command line's stages
    //
    // `CliStartStream::Launcher` emits five signals and no `Session` until the
    // fourth of them (`app/cli/startstream.h:31-36`). Everything before that
    // is a machine being looked for and an application list being read — the
    // first rung of the same ladder, lit by the same property — and everything
    // after it is the widget's path exactly.
    //
    // Both of the launcher's pre-session stages sit on rung 1, *Finding %1 on
    // your private network*, because both of them are that. Rung 2 is the RTSP
    // handshake and reading `/applist` is not it; moving the ladder there to
    // look busier would be the client inventing a stage no one reports, which
    // is the thing the ladder exists to stop.

    function cliSearchingComputer() {
        cliStep = 1
        rung = 1
    }

    function cliSearchingApp() {
        cliStep = 2
        rung = 1
    }

    function cliSessionCreated(name, createdSession) {
        // Upstream's own capitalisation of the application, which it matched
        // case-insensitively (`getAppIndex()`, `app/cli/startstream.cpp:146`).
        appName = name
        session = createdSession
        beginSession()
    }

    function cliAppQuitRequired(running) {
        quitConfirm.running = running
        quitConfirm.open()
    }

    // The launcher hands over one composed English sentence and nothing
    // structured, so *which* failure this is comes from how far it got rather
    // than from what it said. Its four messages
    // (`app/cli/startstream.cpp:89, 126, 133, 137`) go under *Details* like
    // any other fact about the program.
    function cliFailed(text) {
        failed = true
        // Always worth offering: every one of these leaves a machine that may
        // well be reachable by ssh even though it would not stream.
        failActions = ["terminal"]

        if (cliStep >= 3) {
            failHeadline = qsTr("%1 is already streaming to another device, and would not stop.")
                            .arg(machineName)
        }
        else if (cliStep >= 2) {
            failHeadline = qsTr("%1 answered, but is not offering \u201C%2\u201D to stream.")
                            .arg(machineName).arg(appName)
        }
        else {
            // Two causes, and the launcher cannot tell them apart either:
            // `Event::Timedout` in `StateSeekComputer` and the unpaired branch
            // of `Event::ComputerFound` both arrive here with nothing emitted
            // in between (`app/cli/startstream.cpp:80-94, 130-134`). Saying
            // both is what `OmnuvView` does for the same ambiguity rather than
            // guessing one.
            failHeadline = qsTr("This device could not start streaming from %1. It either did not " +
                                "answer on your private network, or it has not been paired with this " +
                                "device yet — open Omnuv and press Play, which pairs it for you.")
                            .arg(machineName)
        }

        addDetail(text)
    }

    function beginSession() {
        session.stageStarting.connect(stageStarting)
        session.stageFailed.connect(stageFailed)
        session.connectionStarted.connect(connectionStarted)
        session.displayLaunchError.connect(displayLaunchError)
        session.quitStarting.connect(quitStarting)
        session.sessionFinished.connect(sessionFinished)
        session.readyForDeletion.connect(sessionReadyForDeletion)

        SystemProperties.waitForAsyncLoad()

        streamLoader.active = true
    }

    StackView.onActivated: {
        // Assigned rather than bound, exactly as upstream's five segues do.
        // `app/gui/main.qml` binds the toolbar's *height* and never its
        // `visible`, so these writes cost nothing there and keep meaning what
        // they meant.
        toolBar.visible = false

        revealTimer.start()

        if (cli) {
            // Upstream's guard, for upstream's reason: a StackView activates a
            // view again when something above it is popped, and a `Launcher`
            // runs once (`app/gui/CliStartStreamSegue.qml:36`).
            if (!cli.isExecuted()) {
                cli.searchingComputer.connect(cliSearchingComputer)
                cli.searchingApp.connect(cliSearchingApp)
                cli.sessionCreated.connect(cliSessionCreated)
                cli.failed.connect(cliFailed)
                cli.appQuitRequired.connect(cliAppQuitRequired)
                cli.execute(ComputerManager)
            }
            return
        }

        beginSession()
    }

    Timer {
        id: revealTimer
        // Upstream waits 100 ms before showing its spinner so the animation
        // does not visibly hang while `Session.exec()` reaches the code that
        // pumps the event loop. With reduced motion there is no animation to
        // hang, so there is nothing to wait for and the ladder appears at once
        // — which is also the only honest thing to do when the marker, rather
        // than the movement, is what says "this is happening".
        interval: Theme.motion ? 100 : 0
        onTriggered: segue.revealed = true
    }

    Timer {
        id: startSessionTimer
        onTriggered: {
            gc()
            session.start()
        }
    }

    Loader {
        id: streamLoader
        active: false
        asynchronous: true
        sourceComponent: Item {}

        onLoaded: {
            hintText.text = qsTr("Press %1 to leave the stream")
                            .arg(SdlGamepadKeyNavigation.getConnectedGamepads() > 0
                                 ? qsTr("Start+Select+L1+R1") : qsTr("Ctrl+Alt+Shift+Q"))

            SdlGamepadKeyNavigation.disable()

            if (!session.initialize(window)) {
                sessionFinished(0)
                sessionReadyForDeletion()
                return
            }

            startSessionTimer.interval = 0

            // Upstream's toasts, kept: these are real warnings about what the
            // stream had to give up (HDR, a codec, a frame rate) and losing
            // them would be losing information, not chrome.
            var yOffset = 0
            for (var i = 0; i < session.launchWarnings.length; i++) {
                var text = session.launchWarnings[i]
                console.warn(text)

                var toast = Qt.createQmlObject('import QtQuick.Controls 2.2; ToolTip {}', parent, '')
                toast.timeout = 3000
                toast.text = text
                toast.y += yOffset
                toast.visible = true

                yOffset = toast.y + toast.padding + toast.height
                startSessionTimer.interval = toast.timeout + 500
            }

            startSessionTimer.start()
        }
    }

    // ----------------------------------------------------------- the ladder
    //
    // Five rows, one lit. **What conveys "this one is happening" differs by
    // whether the person asked for reduced motion, and it is a different
    // control rather than the same control standing still.** A stopped spinner
    // reads as broken, which is the opposite of the thing it has to say; so
    // with motion off the current rung gets a filled marker and the word
    // *now*, which says it without moving. Done rungs carry a tick either way,
    // so the position in the list is readable on its own.

    ColumnLayout {
        id: ladderPanel
        visible: segue.revealed && !segue.failed
        anchors.centerIn: parent
        width: Math.min(segue.width - 96, 520)
        spacing: Theme.spacingTight

        Label {
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.spacingLoose
            text: qsTr("Starting %1 on %2").arg(segue.appName).arg(segue.machineName)
            font.family: Theme.textFamily
            font.pixelSize: Theme.subtitleSize
            font.weight: Theme.strongWeight
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: segue.ladder

            RowLayout {
                id: rungRow
                Layout.fillWidth: true
                spacing: Theme.spacingLoose
                readonly property bool done: index < segue.rung
                readonly property bool current: index === segue.rung

                Item {
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20

                    Label {
                        anchors.centerIn: parent
                        visible: rungRow.done
                        text: Theme.icon.accept
                        font.family: Theme.iconFamily
                        font.pixelSize: Theme.captionSize
                        opacity: 0.6
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        width: 20
                        height: 20
                        // Only where turning is allowed to be the message.
                        visible: rungRow.current && Theme.motion
                        running: visible
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: 8
                        height: 8
                        radius: 4
                        // Filled for the rung in progress, hollow for one not
                        // reached. Under reduced motion this is the whole
                        // indicator, which is why it is a shape and not a
                        // paused animation.
                        visible: !rungRow.done && !(rungRow.current && Theme.motion)
                        color: rungRow.current ? Theme.accent : "transparent"
                        border.width: rungRow.current ? 0 : 1
                        border.color: Theme.systemPalette.text
                        opacity: rungRow.current ? 1 : 0.35
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: modelData
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                    font.weight: rungRow.current ? Theme.strongWeight : Theme.regularWeight
                    wrapMode: Text.WordWrap
                    opacity: rungRow.current ? 1 : (rungRow.done ? 0.7 : 0.4)

                    // Collapses to an instant change under reduced motion,
                    // where the marker beside it is already carrying the
                    // meaning. With motion it is the escort.
                    Behavior on opacity {
                        NumberAnimation {
                            duration: Theme.durationNormal
                            easing.type: Easing.Bezier
                            easing.bezierCurve: Theme.easeEntrance
                        }
                    }
                }

                Label {
                    // The other half of the reduced-motion branch: a word where
                    // the spinner would have been.
                    visible: rungRow.current && !Theme.motion
                    text: qsTr("now")
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.6
                }
            }
        }
    }

    Label {
        id: hintText
        visible: ladderPanel.visible
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 50
        anchors.horizontalCenter: parent.horizontalCenter
        font.family: Theme.textFamily
        font.pixelSize: Theme.bodySize
        opacity: 0.6
        wrapMode: Text.Wrap
    }

    // ---------------------------------------------------------- the failure
    //
    // A sentence, what to do, and the numbers folded away. The headline never
    // carries a code — that is the entire point of this screen existing.

    ColumnLayout {
        id: failPanel
        visible: segue.failed
        anchors.centerIn: parent
        width: Math.min(segue.width - 96, 560)
        spacing: Theme.spacingLoose

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingLoose

            Label {
                Layout.alignment: Qt.AlignTop
                text: Theme.icon.error
                font.family: Theme.iconFamily
                font.pixelSize: Theme.subtitleSize
            }

            Label {
                Layout.fillWidth: true
                text: segue.failHeadline
                font.family: Theme.textFamily
                font.pixelSize: Theme.subtitleSize
                font.weight: Theme.strongWeight
                lineHeight: Theme.subtitleLineHeight
                lineHeightMode: Text.FixedHeight
                wrapMode: Text.WordWrap
            }
        }

        Label {
            Layout.fillWidth: true
            visible: text !== ""
            text: segue.failExtra
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            lineHeight: Theme.bodyLineHeight
            lineHeightMode: Text.FixedHeight
            wrapMode: Text.WordWrap
            opacity: 0.8
        }

        Flow {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingTight
            spacing: Theme.spacing

            Button {
                visible: segue.failActions.indexOf("settings") >= 0
                text: qsTr("Change stream settings")
                // Upstream's page, in upstream's style, which is where the
                // resolution, frame rate and codec actually live. A sheet of
                // the six that matter is a later phase; sending a person to
                // the real thing is better than sending them nowhere.
                onClicked: stackView.push("qrc:/gui/SettingsView.qml")
            }

            Button {
                visible: segue.failActions.indexOf("terminal") >= 0 && segue.machineHost !== ""
                text: qsTr("Open a terminal instead")
                onClicked: Omnuv.openTerminal(segue.machineHost, segue.machineUser)
            }

            Button {
                // Always offered when the network is down, whatever failed:
                // it is the one thing that is worth doing in every case, and
                // hiding it when it is up keeps it from being noise.
                visible: Omnuv.tunnel.available && !Omnuv.tunnel.connected
                enabled: !Omnuv.tunnel.busy
                text: Omnuv.tunnel.busy ? qsTr("Joining…") : qsTr("Join this device to the network")
                onClicked: Omnuv.tunnel.join()
            }

            Button {
                // Nothing on the command line can start over: a `Launcher` is
                // single-shot (`isExecuted()`, `app/cli/startstream.cpp:219`)
                // and there is no machine list underneath to go back to. The
                // retry there is running the command again, and a button that
                // cannot do what it says is worse than no button.
                visible: !segue.cli
                text: qsTr("Try again")
                highlighted: true
                onClicked: segue.retryRequested()
            }

            Button {
                text: qsTr("Close")
                highlighted: segue.cli !== null
                onClicked: segue.leave()
            }
        }

        Button {
            Layout.topMargin: Theme.spacingTight
            flat: true
            checkable: true
            id: detailsToggle
            text: checked ? qsTr("Hide details") : qsTr("Details")
        }

        Label {
            Layout.fillWidth: true
            visible: detailsToggle.checked
            text: segue.failDetails
            font.family: "monospace"
            font.pixelSize: Theme.captionSize
            wrapMode: Text.WrapAnywhere
            opacity: 0.7
        }
    }

    // ------------------------------------------------- something else is on
    //
    // Upstream's command-line segue asks before stopping whatever the machine
    // is already streaming (`app/gui/CliStartStreamSegue.qml:76-90`), and the
    // `stream` verb would lose that branch if this file did not carry it. Only
    // ever opened by `cliAppQuitRequired`, so it never appears on the widget's
    // path — `OmnuvView.startStream()` has no equivalent and calls straight
    // through, which is a real gap on that side and not one this file can
    // close.
    //
    // Declining leaves, which on the command line is quitting, because there
    // is nothing else this process was started to do.
    Dialog {
        id: quitConfirm
        property string running

        anchors.centerIn: parent
        width: Math.min(segue.width - 96, 460)
        modal: true
        standardButtons: Dialog.Yes | Dialog.No
        title: qsTr("%1 is already streaming").arg(segue.machineName)

        onAccepted: {
            segue.cliStep = 3
            segue.cli.quitRunningApp()
        }
        onRejected: segue.leave()

        Label {
            width: parent.width
            text: qsTr("%1 is running on %2. Stopping it to start \u201C%3\u201D will lose anything " +
                       "it has not saved.")
                   .arg(quitConfirm.running).arg(segue.machineName).arg(segue.appName)
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            wrapMode: Text.WordWrap
        }
    }
}
