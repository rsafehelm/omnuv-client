// Omnuv: one machine, as a card.
//
// Everything on it is a field Core sent. Nothing here infers a state from
// another state, and nothing here is drawn as done because a constant said so
// — the launch ladder in particular names, per step, the field that confirms
// it, and draws a step with no evidence as *unknown* rather than as done or
// pending. That is the same rule the web console's `ladder()` follows, and the
// two vocabularies are deliberately identical: a person who reads "Private
// network — not observed" in the browser and "Private network — not observed"
// here has learned one thing, not two.
//
// Used as a GridView delegate, so `model` and `index` are the delegate's own
// context. It reports what a person pressed and decides nothing: the view above
// owns connecting, because connecting is upstream's machinery and this file is
// a rendering.

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import Omnuv 1.0

ItemDelegate {
    id: card

    // The wall clock, ticked by the view that owns the list. Every card shows
    // the same second, so there is one timer for the screen rather than one per
    // card — and a card that is not on a screen is not driving anything.
    property double now: 0

    // Space kept free on the right and below, so cards laid edge to edge in
    // a grid cell still stand apart. The card fills its cell; the gap is
    // part of it and draws nothing.
    property int gap: 0

    readonly property string summaryText: model.summary
    rightInset: gap
    bottomInset: gap
    clip: true

    signal primaryActivated()
    signal chooseAppRequested()
    signal terminalRequested()
    signal settingsRequested()

    height: body.implicitHeight + 2 * Theme.padding
    onClicked: if (model.ready) card.primaryActivated()

    // ---- The evidence, named once ---------------------------------------
    //
    // These are the four tests the console's `ladder()` makes, under the same
    // names, so the two can be compared by reading them side by side. Core's
    // `friendly()` collapses PENDING and PROVISIONING into one word, which is
    // why `booted` tests for "Starting" rather than for a runtime state the
    // buyer API does not publish.
    readonly property bool running: model.status === "Running"
    readonly property bool booted: running || model.status === "Starting"
    readonly property bool addressed: model.privateIp !== ""
    readonly property bool failed: model.status === "Needs attention"

    // Shown while something is happening, and the test is Core's object rather
    // than our reading of the status word — an operation with no word yet is
    // still an operation.
    readonly property bool launching: model.operating || model.status === "Starting"

    /**
     * The launch, as far as it can be *observed*.
     *
     * Kept line for line with `console-buyer/.../instances/+page.svelte`:
     *
     *     Requested and placed   the row exists, and its provider was chosen in
     *                            the same transaction — so placement is known
     *                            at the same moment and is one step, not two
     *     Booting                status is Starting or Running
     *     Private network        private_ip is set
     *     Running                status is Running
     *
     * There is no "Creating disk" and no "Attaching GPU": nothing reports them,
     * and a step nothing reports is a step that would always be a guess.
     */
    function ladder() {
        return [
            { label: qsTr("Requested and placed"), state: "done", note: "" },
            {
                label: qsTr("Booting"),
                state: failed ? "failed" : running ? "done" : booted ? "active" : "unknown",
                note: (!running && booted) ? model.waitingOn : ""
            },
            {
                // Not "pending": until an address is assigned nobody has
                // looked, and `unknown` is the honest word for that.
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

    // An action under way is progress, not a fault. Anything Core can say has
    // to be named here, or it falls through to the colour that means trouble —
    // which is why Restarting and Stopping are listed rather than assumed.
    function statusColour(status) {
        switch (status) {
        case "Running":  return Theme.fillSuccess
        case "Stopped":
        case "Deleting": return Theme.fillNeutral
        case "Starting":
        case "Restarting":
        case "Stopping": return Theme.fillCaution
        default:         return Theme.fillCritical   // "Needs attention"
        }
    }

    /**
     * How long the action Core has in flight has been running.
     *
     * Empty rather than a number when the timestamp did not parse. A clock
     * that cannot read the time says nothing; it does not print "NaNs", which
     * is what an unguarded `getTime()` on an invalid date puts on the card.
     */
    function elapsed() {
        var at = model.since.getTime()
        if (isNaN(at)) {
            return ""
        }
        var s = Math.max(0, Math.round((now - at) / 1000))
        return s < 60 ? qsTr("%1s").arg(s)
                      : qsTr("%1m %2s").arg(Math.floor(s / 60)).arg(s % 60)
    }

    /**
     * When the provider last *looked*, shown only when it did.
     *
     * Empty when Core sent no observation, or when its sweep did not cover
     * machines — Core decides that, and a client must not invent a timestamp.
     * An incomplete sweep says so instead of dating the claim: it can prove a
     * machine was there and can never prove one was not.
     */
    function observed() {
        if (!model.observed) {
            return ""
        }
        if (!model.observationComplete) {
            return qsTr("last observation incomplete")
        }
        var at = model.observedAt.getTime()
        if (isNaN(at)) {
            // Core said it had looked and gave a time nothing could read. That
            // is a fact about the report, not about the machine, and it is
            // better said than dated.
            return qsTr("last observation incomplete")
        }
        var s = Math.max(0, Math.round((now - at) / 1000))
        return s < 60 ? qsTr("observed %1s ago").arg(s)
                      : qsTr("observed %1m ago").arg(Math.floor(s / 60))
    }

    // WinUI has a card; Qt Quick Controls does not, so this one surface is ours
    // to paint. The colours are Microsoft's and translucent by design — they
    // are meant to let the Mica behind the window through.
    //
    // Hover and focus are one border rather than a second fill, because the
    // fill is carrying the window's backdrop and a solid hover state would
    // punch a hole in it. Two pixels of accent on focus is Windows' own focus
    // ring.
    background: Rectangle {
        radius: Theme.radiusControl
        color: Theme.fillCard
        border.width: card.activeFocus ? 2 : 1
        border.color: (card.hovered || card.activeFocus) ? Theme.accent : Theme.strokeCard

        Behavior on border.color {
            ColorAnimation {
                duration: Theme.durationFaster
                easing.type: Easing.Bezier
                easing.bezierCurve: Theme.easePointToPoint
            }
        }
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: Theme.padding
        anchors.rightMargin: Theme.padding + card.gap
        anchors.bottomMargin: Theme.padding + card.gap
        spacing: Theme.spacing

        // ---- Name, and the state in a word ------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingLoose

            Label {
                text: model.name
                Layout.maximumWidth: card.width * 0.5
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                font.weight: Theme.strongWeight
                elide: Label.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: model.region
                font.family: Theme.textFamily
                font.pixelSize: Theme.captionSize
                opacity: 0.6
                elide: Label.ElideRight
            }

            // The dot and the word, never the dot alone. Under high contrast
            // Windows sets every system fill colour to the same red on
            // purpose, so the word is the only thing left carrying meaning —
            // and a person who cannot tell amber from green is in the same
            // position on an ordinary display.
            Pill {
                tone: card.statusColour(model.status)
                dot: true
                text: model.status
            }
        }

        // ---- What it is, and where ---------------------------------------
        //
        // Core's summary, one chip per fact. Split
        // on the separator Core itself puts between the facts, so a summary
        // with three facts or five draws three chips or five.
        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacingTight

            Repeater {
                // Through the card: inside a Repeater, `model` is the
                // Repeater's own property, not this machine.
                model: card.summaryText !== "" ? card.summaryText.split(" \u00B7 ") : []

                Pill {
                    text: modelData
                }
            }

        }

        // The name it answers to on the project network — the one address
        // this application ever connects to. Empty until one is assigned.
        Label {
            Layout.fillWidth: true
            visible: model.host !== ""
            text: model.host
            font.family: "monospace"
            font.pixelSize: Theme.captionSize
            opacity: 0.7
            elide: Label.ElideRight
        }

        // ---- The ladder, while something is happening --------------------
        //
        // Compact, because the card is a grid cell: the four steps are four
        // segments, and the one step that matters — the first that is not
        // done — is named beneath them with its note and its clock. The
        // steps, their states and their words are still `ladder()`'s, so the
        // console and this card say the same thing; only the drawing folded.
        ColumnLayout {
            id: ladderView
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingTight
            spacing: Theme.spacingTight
            visible: card.launching

            readonly property var steps: card.launching ? card.ladder() : []

            // The step to name: a failure first, then the one in progress,
            // then one nobody has observed, then the next still to come.
            readonly property var current: {
                var order = ["failed", "active", "unknown", "pending"]
                for (var o = 0; o < order.length; o++) {
                    for (var i = 0; i < steps.length; i++) {
                        if (steps[i].state === order[o]) {
                            return steps[i]
                        }
                    }
                }
                return steps.length > 0 ? steps[steps.length - 1] : null
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 2
                Accessible.role: Accessible.ProgressBar
                Accessible.name: {
                    var parts = []
                    for (var i = 0; i < ladderView.steps.length; i++) {
                        parts.push(ladderView.steps[i].label + ": " + ladderView.steps[i].state)
                    }
                    return parts.join(", ")
                }

                Repeater {
                    model: ladderView.steps

                    Rectangle {
                        id: segment
                        Layout.fillWidth: true
                        implicitHeight: 4
                        radius: 2
                        readonly property string state_: modelData.state
                        color: state_ === "done" ? Theme.fillSuccess
                             : state_ === "failed" ? Theme.fillCritical
                             : state_ === "active" ? Theme.accent
                             : state_ === "unknown" ? "transparent"
                             : Theme.strokeCard
                        border.width: state_ === "unknown" ? 1 : 0
                        border.color: Theme.fillNeutral

                        // The one moving thing on the card: the active step
                        // dims and returns once a second, on the screen's own
                        // clock. A continuous animation redraws the window
                        // sixty times a second for as long as a machine is
                        // starting — measured at 18 % of a core in the Linux
                        // loop — where this redraws for a sixth of a second.
                        // Under reduced motion it holds still.
                        opacity: state_ === "active" && Theme.motion
                                 && Math.floor(card.now / 1000) % 2 === 1 ? 0.45 : 1
                        Behavior on opacity {
                            NumberAnimation { duration: Theme.durationFast }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing

                Label {
                    Layout.fillWidth: true
                    // A failed step is said in words, not only in red.
                    text: !ladderView.current ? ""
                          : ladderView.current.state === "failed" ? qsTr("Stopped at: %1").arg(ladderView.current.label)
                          : ladderView.current.label
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                    font.weight: Theme.strongWeight
                    elide: Label.ElideRight
                }

                // How long, and how many times — only when Core has an
                // operation to date them from. Attempt 1 is not news.
                Label {
                    visible: model.operating
                    text: !model.operating ? ""
                          : model.attempt > 1 ? qsTr("%1 · attempt %2").arg(card.elapsed()).arg(model.attempt)
                          : card.elapsed()
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.6
                }
            }

            // The agent's own words when it said any, and "not observed" when
            // nobody looked. Colour is never the only carrier of either.
            Label {
                Layout.fillWidth: true
                text: !ladderView.current ? ""
                      : ladderView.current.note !== "" ? ladderView.current.note
                      : ladderView.current.state === "unknown" ? qsTr("not observed") : ""
                visible: text !== ""
                font.family: Theme.textFamily
                font.pixelSize: Theme.captionSize
                opacity: 0.6
                elide: Label.ElideRight
            }
        }

        // ---- What went wrong, in Core's words ----------------------------
        //
        // A badge that says "Needs attention" and nothing else is the same
        // defect as a numeric code: it tells a person something is wrong and
        // gives them nowhere to go. Core already composes a sentence; it is
        // shown verbatim rather than translated into one of ours.
        Label {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingTight
            visible: model.lastError !== ""
            text: model.lastError
            font.family: Theme.textFamily
            font.pixelSize: Theme.captionSize
            color: Theme.fillCritical
            wrapMode: Text.WordWrap
            // A grid cell has room for two lines; the whole sentence is one
            // hover away, because a truncated reason is still Core's reason.
            maximumLineCount: 2
            elide: Label.ElideRight
            ToolTip.visible: truncated && errorHover.containsMouse
            ToolTip.text: model.lastError
            // Hover only: it takes no buttons, so a press still reaches the card.
            MouseArea {
                id: errorHover
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
            }
        }

        // The actions sit on the card's floor whatever is above them, so a
        // row of cards reads as a row.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        // ---- One primary action, and the rest under a menu ----------------
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingTight
            spacing: Theme.spacing

            // When the provider last looked, when it has. The test is the
            // model's field rather than `observed()`, which reads the clock.
            Label {
                Layout.fillWidth: true
                text: model.observed ? card.observed() : ""
                font.family: Theme.textFamily
                font.pixelSize: Theme.captionSize
                opacity: 0.5
                elide: Label.ElideRight
            }

            Button {
                // Play when the machine streams something, Terminal when it
                // does not. Never both as buttons: a card with two equal
                // actions makes a person choose before they know what they
                // want.
                text: model.streamed ? qsTr("Play") : qsTr("Terminal")
                enabled: model.ready
                highlighted: true
                onClicked: card.primaryActivated()
            }

            // One primary action, and everything else behind it. The grid
            // Play used to open lives here now — a choice made once a month
            // does not deserve to be the screen between a person and their
            // machine.
            ToolButton {
                id: moreButton
                text: Theme.iconsInstalled ? Theme.icon.more : qsTr("More")
                font.family: Theme.iconsInstalled ? Theme.iconFamily : Theme.textFamily
                font.pixelSize: Theme.bodySize
                hoverEnabled: true
                // Only where there is more. A machine whose one action is a
                // terminal has nothing under here, and a menu that opens on an
                // empty list is worse than no menu.
                visible: model.streamed
                enabled: model.ready
                // Screen readers and the tooltip get a word; the glyph is only
                // for the eye.
                Accessible.name: qsTr("More actions for %1").arg(model.name)
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: moreMenu.open()

                Menu {
                    id: moreMenu
                    y: moreButton.height

                    MenuItem {
                        text: qsTr("Choose what to stream")
                        onTriggered: card.chooseAppRequested()
                    }

                    // The other way in, for a machine whose primary action is
                    // the stream. An SSH session is still how a person fixes a
                    // rig that will not stream, which is exactly when they need
                    // it most.
                    MenuItem {
                        text: qsTr("Terminal")
                        onTriggered: card.terminalRequested()
                    }

                    MenuItem {
                        text: qsTr("Stream settings")
                        onTriggered: card.settingsRequested()
                    }

                    // ponytail: the design's menu also lists *Console in
                    // browser*, *Stop* and *Delete*, and none is a rendering
                    // problem:
                    //
                    //   Stop, Delete         need buyer endpoints this
                    //       application does not have — it makes four calls and
                    //       the console makes thirty-one. Closing that gap is
                    //       I5, with a test that fails on any endpoint the
                    //       console has and the client lacks.
                    //   Console in browser   needs the console's own address,
                    //       which this application is never told. It holds the
                    //       *API* address, and turning one into the other is
                    //       the client inventing a name — the habit
                    //       `private_name` was taken away from it to break.
                }
            }
        }
    }
}
