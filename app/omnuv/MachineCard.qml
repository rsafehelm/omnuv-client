// Omnuv: one instance, as a card — the web's card, 1:1 (the Instances
// redesign, 3 October 2026; docs/plans/recipes-in-instances.md sections 2
// and 4, in the omnuv repository).
//
//   header   mark · name · Protected · what it is · Core's word · price
//   app      (an app only) its line: the step while it installs, the address
//            once it runs, what went wrong when it did not
//   access   (plain Linux only) the ssh line, with Copy command
//   specs    vCPU · Memory · Disk · GPU, four tiles
//   footer   hint · secondary · primary · More (⋯)
//
// **The word is Core's, verbatim** (`recipes::word`): Deploying, Installing,
// Running, Ready, Needs attention, Starting, Stopping, Restarting, Stopped.
// The card draws it and decides nothing: no step it was not told, no verdict
// from a clock — "taking longer than usual" past twice the measured install
// is a note, never a word. Everything here is a field Core sent.
//
// Used as a GridView delegate, so `model` is the delegate's own context. It
// reports what a person pressed; the view above acts.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omnuv 1.0

ItemDelegate {
    id: card

    // The wall clock, ticked by the view: one timer for the screen.
    property double now: 0
    // Space kept free on the right and below, so cards laid edge to edge in a
    // grid cell stand apart.
    property int gap: 0

    rightInset: gap
    bottomInset: gap
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    signal primaryActivated()
    signal chooseAppRequested()
    signal terminalRequested()
    signal settingsRequested()
    signal deleteRequested()
    signal powerRequested(string action)

    readonly property bool deletionProtected: model.protected === true

    // ---- What the card is about, named once ----------------------------
    readonly property string word: model.status
    readonly property bool app: model.hasApp === true
    readonly property string what: app ? model.appName : model.image
    readonly property bool usable: word === "Running" || word === "Ready"
    readonly property bool installing: word === "Installing"
    readonly property bool deploying: word === "Deploying"
    readonly property bool stopped: word === "Stopped"
    readonly property bool attention: word === "Needs attention"
    readonly property bool moving: word === "Starting" || word === "Restarting" || word === "Stopping"
    readonly property bool progress: deploying || installing || moving
    readonly property bool web: model.access === "web" || model.webPort > 0
    readonly property bool stream: model.access === "stream" || model.streamed
    // A plain Linux machine is reached by its ssh line (section 2).
    readonly property bool shell: !app && !stream && !web
    readonly property var specs: [
        { value: String(model.vcpus), unit: qsTr("vCPU") },
        { value: model.memoryGib + " GiB", unit: qsTr("Memory") },
        { value: model.diskGib + " GiB", unit: qsTr("Disk") },
        { value: model.gpuModel !== "" ? (model.gpuCount > 1 ? model.gpuCount + " × " : "") + model.gpuModel : "—",
          unit: qsTr("GPU") }
    ]

    // ---- A plain machine's launch, as far as it can be observed ----------
    //
    // Kept line for line with the web console's `ladder()`: each step names
    // the field that confirms it, and a step nobody has observed is drawn as
    // unknown, never as done. An app's install has its own steps above.
    readonly property bool running: model.status === "Running"
    readonly property bool booted: running || model.status === "Starting"
    readonly property bool addressed: model.privateIp !== ""
    readonly property bool failed: model.status === "Needs attention"
    readonly property bool launching: !app && (model.operating || deploying || model.status === "Starting")

    function ladder() {
        return [
            { label: qsTr("Requested and placed"), state: "done", note: "" },
            {
                label: qsTr("Booting"),
                state: failed ? "failed" : running ? "done" : booted ? "active" : "unknown",
                note: (!running && booted) ? model.waitingOn : ""
            },
            {
                label: qsTr("Private network"),
                state: addressed ? "done" : running ? "unknown" : "pending",
                note: ""
            },
            {
                label: qsTr("Running"),
                state: running ? "done" : failed ? "failed" : "pending",
                note: ""
            }
        ]
    }

    // The one grouping of Core's words, the same as the tray's
    // (`Machine::health()`); `checks.sh` holds the two together.
    function statusColour(status) {
        switch (status) {
        case "Running":
        case "Ready":    return Theme.fillSuccess
        case "Stopped":
        case "Deleting": return Theme.fillNeutral
        case "Deploying":
        case "Installing":
        case "Starting":
        case "Restarting":
        case "Stopping": return Theme.fillCaution
        default:         return Theme.fillCritical   // "Needs attention"
        }
    }

    // "4 min" or "40 s": how long the app has been at it.
    function since(at) {
        var t = at && at.getTime ? at.getTime() : NaN
        if (isNaN(t)) return ""
        var s = Math.max(0, Math.round((now - t) / 1000))
        return s < 60 ? qsTr("%1 s").arg(s) : qsTr("%1 min").arg(Math.floor(s / 60))
    }
    // D-7: past twice a measured install, and never on a guess.
    readonly property bool takingLonger: {
        if (!installing || model.typicalSecs <= 0) return false
        var t = model.appSince && model.appSince.getTime ? model.appSince.getTime() : NaN
        return !isNaN(t) && now - t > 2 * model.typicalSecs * 1000
    }

    // The app section's line, in the state table's words.
    function appLine() {
        if (deploying) return qsTr("Getting an instance ready for %1.").arg(model.appName)
        if (installing) return model.stepN > 0
            ? qsTr("Step %1 of %2").arg(model.stepN).arg(model.stepOf) + (model.stepLabel !== "" ? " · " + model.stepLabel : "")
            : qsTr("Installing %1.").arg(model.appName)
        if (attention) return model.stepN > 0
            ? qsTr("%1 did not finish installing. It stopped at step %2 of %3%4.")
                .arg(model.appName).arg(model.stepN).arg(model.stepOf)
                .arg(model.stepLabel !== "" ? ", " + model.stepLabel : "")
            : qsTr("%1 needs attention.").arg(model.appName)
        if (word === "Starting" || word === "Restarting") return qsTr("Starting. %1 comes back with it.").arg(model.appName)
        if (word === "Stopping") return qsTr("Stopping. %1 stops answering.").arg(model.appName)
        if (stopped) return qsTr("Stopped. Its disk and everything %1 holds are kept.").arg(model.appName)
        return model.firstUse
    }

    // The address a person copies: Core's private name, and the port for a
    // web app. The long name-by-id (`host`) is what the app connects to.
    readonly property string address: model.shortHost !== ""
        ? model.shortHost + (web && model.webPort > 0 ? ":" + model.webPort : "") : ""

    // ---- Primary and secondary, section 2's table ------------------------
    readonly property string primaryText: stopped ? qsTr("Start")
        : attention ? qsTr("Terminal")
        : stream ? qsTr("Play")
        : web ? qsTr("Open ↗")
        : qsTr("Terminal")
    // Disabled, never hidden, while it cannot be used; the reason is the hint.
    // A machine that needs attention is still reached by Terminal when it has
    // an address: that is how a person looks at what went wrong.
    readonly property bool primaryBlocked: stopped ? Omnuv.ordering
        : attention ? model.host === ""
        : !model.ready
    readonly property string hint: {
        if (stopped || attention) return ""
        if (deploying || installing) return app ? qsTr("Opens once it has installed.") : qsTr("Available once it is running.")
        if (moving) return qsTr("Available once it is running.")
        if (!model.ready && usable) return qsTr("Waiting for its address on your network.")
        return ""
    }
    readonly property string secondaryText: stopped || attention ? qsTr("Delete…")
        : stream ? qsTr("Choose what to stream")
        : web ? qsTr("Terminal")
        : ""
    function secondary() {
        if (stopped || attention) card.deleteRequested()
        else if (stream) card.chooseAppRequested()
        else if (web) card.terminalRequested()
    }
    function primary() {
        if (primaryBlocked) return
        if (stopped) card.powerRequested("start")
        else if (attention) card.terminalRequested()
        else card.primaryActivated()
    }

    onClicked: if (model.ready && !stopped && !attention) card.primaryActivated()

    Accessible.role: Accessible.Grouping
    Accessible.name: [model.name, what, word,
                      installing && model.stepN > 0 ? qsTr("step %1 of %2").arg(model.stepN).arg(model.stepOf) : ""]
                     .filter(function (p) { return p !== "" }).join(", ")

    // The insets above keep the gap; the frame fills what is left.
    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: 16
            color: card.hovered ? Theme.fillCardHover : Theme.fillCard
            border.width: card.visualFocus || card.activeFocus ? 2 : 1
            border.color: card.visualFocus || card.activeFocus ? Theme.accent
                        : card.hovered ? Theme.strokeCardHover : Theme.strokeCard
        }
    }

    contentItem: Item {
        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.topMargin: 20
            anchors.rightMargin: 20 + card.gap
            anchors.bottomMargin: 20 + card.gap
            spacing: 14

            // ---- Header -------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                WorkloadMark {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: 48
                    Layout.preferredHeight: 48
                    workload: model.workload
                    hasGpu: model.hasGpu
                    asleep: card.stopped
                    Accessible.ignored: true
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    RowLayout {
                        id: nameRow
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            // Its own width, bounded by the card's, which the
                            // grid fixes: a bound on the row it sits in would
                            // send the layout round again (Qt aborts it).
                            Layout.maximumWidth: Math.max(48, card.width - card.gap - 40 - 60 - 12 - wordColumn.implicitWidth
                                                          - (protectedPill.visible ? protectedPill.implicitWidth + 8 : 0))
                            text: model.name
                            font.family: Theme.displayFamily
                            font.pixelSize: 18
                            font.weight: Theme.strongWeight
                            elide: Label.ElideRight
                        }
                        Pill {
                            id: protectedPill
                            visible: card.deletionProtected
                            text: qsTr("Protected")
                            Accessible.description: qsTr("Protected against deletion")
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: card.what + (model.gpuModel !== "" ? " · " + model.gpuModel : "")
                        font.family: Theme.textFamily
                        font.pixelSize: 13
                        opacity: 0.7
                        elide: Label.ElideRight
                    }
                }

                ColumnLayout {
                    id: wordColumn
                    Layout.alignment: Qt.AlignTop
                    spacing: 6

                    // The word, in a pill: Core's, verbatim.
                    Rectangle {
                        objectName: "wordBadge"
                        Layout.alignment: Qt.AlignRight
                        implicitHeight: 26
                        implicitWidth: wordRow.implicitWidth + 20
                        radius: 13
                        readonly property color tone: card.statusColour(card.word)
                        color: Qt.rgba(tone.r, tone.g, tone.b, Theme.onDarkSurface ? 0.22 : 0.12)
                        Accessible.role: Accessible.StaticText
                        Accessible.name: card.word

                        Row {
                            id: wordRow
                            anchors.centerIn: parent
                            spacing: 6
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 7
                                height: 7
                                radius: 3.5
                                color: parent.parent.tone
                                // Under way, it breathes once a second on the
                                // screen's own clock; still under reduced motion.
                                opacity: card.progress && Theme.motion && Math.floor(card.now / 1000) % 2 === 1 ? 0.35 : 1
                                Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
                            }
                            Label {
                                text: card.word
                                color: parent.parent.tone
                                font.family: Theme.textFamily
                                font.pixelSize: 13
                                font.weight: Theme.strongWeight
                            }
                        }
                    }
                    Label {
                        Layout.alignment: Qt.AlignRight
                        visible: model.price !== ""
                        text: qsTr("€%1/h").arg(model.price)
                        font.family: Theme.monoFamily
                        font.pixelSize: 12
                        opacity: 0.7
                    }
                }
            }

            // ---- The app ------------------------------------------------
            Rectangle {
                objectName: "appSection"
                Layout.fillWidth: true
                visible: card.app && (card.appLine() !== "" || card.address !== "")
                implicitHeight: appColumn.implicitHeight + 24
                radius: 12
                color: card.attention ? Qt.rgba(Theme.fillCaution.r, Theme.fillCaution.g, Theme.fillCaution.b, 0.12)
                                      : Theme.fillSubtle

                ColumnLayout {
                    id: appColumn
                    x: 12
                    y: 12
                    width: parent.width - 24
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        text: card.appLine()
                        visible: text !== ""
                        wrapMode: Text.WordWrap
                        maximumLineCount: 3
                        elide: Label.ElideRight
                        font.family: Theme.textFamily
                        font.pixelSize: 14
                        font.weight: card.installing ? Theme.strongWeight : Theme.regularWeight
                        color: card.attention ? Theme.fillCaution : palette.windowText
                    }

                    // Where the install is, as a bar: half a step for the step
                    // under way, so it never reads as done before it is.
                    Rectangle {
                        Layout.fillWidth: true
                        visible: card.installing || card.deploying
                        implicitHeight: 4
                        radius: 2
                        color: "transparent"
                        border.width: 1
                        border.color: Theme.strokeCard
                        Rectangle {
                            height: parent.height
                            radius: 2
                            color: Theme.accent
                            width: card.installing && model.stepOf > 0
                                   ? parent.width * Math.max(0, model.stepN - 0.5) / model.stepOf
                                   : parent.width * 0.12
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: card.installing && text !== ""
                        text: [card.since(model.appSince) !== "" ? qsTr("%1 so far").arg(card.since(model.appSince)) : "",
                               model.typicalSecs > 0 ? qsTr("usually ~%1 min").arg(Math.max(1, Math.round(model.typicalSecs / 60))) : ""]
                              .filter(function (p) { return p !== "" }).join(" · ")
                        font.family: Theme.textFamily
                        font.pixelSize: 12
                        opacity: 0.6
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: card.takingLonger
                        text: qsTr("Taking longer than usual. Still being watched.")
                        font.family: Theme.textFamily
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }

                    // Once it runs: where it answers, with Copy address.
                    RowLayout {
                        Layout.fillWidth: true
                        visible: card.usable && card.address !== ""
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: card.web ? qsTr("%1 · port %2").arg(model.name).arg(model.webPort) : card.address
                            font.family: card.web ? Theme.textFamily : Theme.monoFamily
                            font.pixelSize: 13
                            elide: Label.ElideMiddle
                        }
                        Button {
                            flat: true
                            text: qsTr("Copy address")
                            Accessible.name: qsTr("Copy the address of %1").arg(model.name)
                            onClicked: { Omnuv.copyText(card.address); copied.show() }
                        }
                    }

                    // What the instance said, folded (Needs attention).
                    ToolButton {
                        id: disclosure
                        visible: card.attention && (model.attention !== "" || model.lastError !== "")
                        checkable: true
                        text: (checked ? "▾ " : "▸ ") + qsTr("What the instance said")
                        font.pixelSize: 13
                        Accessible.name: qsTr("What the instance said")
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: disclosure.visible && disclosure.checked
                        text: model.attention !== "" ? model.attention : model.lastError
                        wrapMode: Text.WordWrap
                        font.family: Theme.textFamily
                        font.pixelSize: 12
                        opacity: 0.8
                    }
                }
            }

            // ---- A plain machine: the ssh line, or Core's reason --------
            RowLayout {
                Layout.fillWidth: true
                visible: card.shell && model.shortHost !== "" && !card.attention && !card.stopped
                spacing: 8
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 32
                    radius: 8
                    color: Theme.fillSubtle
                    Label {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        verticalAlignment: Text.AlignVCenter
                        text: "ssh " + model.user + "@" + model.shortHost
                        font.family: Theme.monoFamily
                        font.pixelSize: 12
                        elide: Label.ElideMiddle
                    }
                }
                Button {
                    flat: true
                    text: qsTr("Copy command")
                    Accessible.name: qsTr("Copy the ssh command for %1").arg(model.name)
                    onClicked: { Omnuv.copyText("ssh " + model.user + "@" + model.shortHost); copied.show() }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: !card.app && text !== "" && (card.attention || card.deploying || card.stopped)
                text: card.stopped ? qsTr("Stopped. Its disk is kept.")
                      : model.lastError !== "" ? model.lastError : model.waitingOn
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Label.ElideRight
                font.family: Theme.textFamily
                font.pixelSize: 13
                color: card.attention ? Theme.fillCritical : palette.windowText
                opacity: card.attention ? 1 : 0.7
            }

            // The ladder, while a plain machine launches: four segments and
            // the step that matters, in the console's words.
            ColumnLayout {
                id: ladderView
                Layout.fillWidth: true
                visible: card.launching
                spacing: 4
                readonly property var steps: card.launching ? card.ladder() : []
                readonly property var current: {
                    var order = ["failed", "active", "unknown", "pending"]
                    for (var o = 0; o < order.length; o++)
                        for (var i = 0; i < steps.length; i++)
                            if (steps[i].state === order[o]) return steps[i]
                    return steps.length > 0 ? steps[steps.length - 1] : null
                }
                Label {
                    Layout.fillWidth: true
                    text: ladderView.current ? (ladderView.current.state === "failed" ? qsTr("Stopped at %1").arg(ladderView.current.label)
                                                : ladderView.current.label + (ladderView.current.state === "unknown" ? qsTr(" \u00B7 not observed") : ""))
                          : ""
                    font.pixelSize: 13
                    opacity: 0.8
                    elide: Label.ElideRight
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    Accessible.role: Accessible.ProgressBar
                    Accessible.name: ladderView.steps.map(function (s) { return s.label + ": " + s.state }).join(", ")
                    Repeater {
                        model: ladderView.steps
                        Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 4
                            radius: 2
                            color: modelData.state === "done" ? Theme.fillSuccess
                                 : modelData.state === "failed" ? Theme.fillCritical
                                 : modelData.state === "active" ? Theme.accent
                                 : modelData.state === "unknown" ? "transparent" : Theme.fillSubtle
                            border.width: modelData.state === "unknown" ? 1 : 0
                            border.color: Theme.fillNeutral
                        }
                    }
                }
            }

            // ---- Specs ---------------------------------------------------
            GridLayout {
                Layout.fillWidth: true
                columns: card.width < 380 ? 2 : 4
                columnSpacing: 8
                rowSpacing: 8

                Repeater {
                    model: card.specs
                    Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        implicitHeight: 54
                        radius: 10
                        color: Theme.fillSubtle
                        Column {
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 20
                            spacing: 2
                            Label {
                                width: parent.width
                                text: modelData.value
                                font.family: Theme.textFamily
                                font.pixelSize: 15
                                font.weight: Theme.strongWeight
                                elide: Label.ElideRight
                            }
                            Label {
                                text: modelData.unit
                                font.family: Theme.textFamily
                                font.pixelSize: 12
                                opacity: 0.6
                            }
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }

            // ---- Footer: hint · secondary · primary · More ---------------
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: copied.shown ? qsTr("Copied") : card.hint
                    font.family: Theme.textFamily
                    font.pixelSize: 12
                    opacity: 0.6
                    elide: Label.ElideRight
                    Timer {
                        id: copied
                        property bool shown: false
                        interval: 2000
                        function show() { shown = true; restart() }
                        onTriggered: shown = false
                    }
                }

                Button {
                    objectName: "secondaryAction"
                    visible: card.secondaryText !== ""
                    text: card.secondaryText
                    enabled: !(card.stopped || card.attention) || !Omnuv.ordering
                    onClicked: card.secondary()
                }

                Button {
                    objectName: "primaryAction"
                    text: card.primaryText
                    highlighted: !card.primaryBlocked
                    // Disabled, but still focusable, with its reason read out.
                    opacity: card.primaryBlocked ? 0.5 : 1
                    Accessible.name: card.stream && !card.stopped && !card.attention ? qsTr("Play on %1").arg(model.name)
                                   : card.web && !card.stopped && !card.attention
                                     ? qsTr("Open %1 on %2").arg(model.appName !== "" ? model.appName : model.name).arg(model.name)
                                   : qsTr("%1 on %2").arg(card.primaryText).arg(model.name)
                    Accessible.description: card.primaryBlocked ? card.hint : ""
                    implicitWidth: Math.max(96, implicitContentWidth + leftPadding + rightPadding)
                    onClicked: card.primary()
                }

                ToolButton {
                    id: moreButton
                    objectName: "moreActions"
                    text: Theme.iconsInstalled ? Theme.icon.more : "⋯"
                    font.family: Theme.iconsInstalled ? Theme.iconFamily : Theme.textFamily
                    font.pixelSize: Theme.bodySize
                    Accessible.name: qsTr("More actions for %1").arg(model.name)
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: moreMenu.open()

                    // Section 2's order: what to use, what it is doing, then
                    // Delete… last.
                    Menu {
                        id: moreMenu
                        y: moreButton.height

                        MenuItem {
                            text: qsTr("Terminal")
                            visible: card.stream && !card.stopped
                            height: visible ? implicitHeight : 0
                            enabled: model.ready
                            onTriggered: card.terminalRequested()
                        }
                        MenuItem {
                            text: qsTr("Stream settings")
                            visible: card.stream && !card.stopped
                            height: visible ? implicitHeight : 0
                            onTriggered: card.settingsRequested()
                        }
                        MenuItem {
                            text: qsTr("Restart")
                            visible: card.usable || card.attention
                            height: visible ? implicitHeight : 0
                            onTriggered: card.powerRequested("reboot")
                        }
                        MenuItem {
                            text: qsTr("Stop")
                            visible: card.usable || card.attention
                            height: visible ? implicitHeight : 0
                            onTriggered: card.powerRequested("stop")
                        }
                        MenuSeparator {}
                        MenuItem {
                            objectName: "deleteMachine"
                            text: qsTr("Delete…")
                            enabled: !Omnuv.ordering
                            onTriggered: card.deleteRequested()
                        }
                    }
                }
            }
        }
    }
}
