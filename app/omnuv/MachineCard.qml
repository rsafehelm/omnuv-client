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
// Used as a ListView delegate, so `model` and `index` are the delegate's own
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
        spacing: Theme.spacing

        // ---- Name, and the state in a word ------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingLoose

            Label {
                Layout.fillWidth: true
                text: model.name
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                font.weight: Theme.strongWeight
                elide: Label.ElideRight
            }

            // The dot and the word, never the dot alone. Under high contrast
            // Windows sets every system fill colour to the same red on
            // purpose, so the word is the only thing left carrying meaning —
            // and a person who cannot tell amber from green is in the same
            // position on an ordinary display.
            RowLayout {
                spacing: Theme.spacingTight

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: Theme.spacing
                    implicitHeight: Theme.spacing
                    radius: Theme.spacing / 2
                    color: card.statusColour(model.status)
                }

                Label {
                    text: model.status
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                }
            }
        }

        // ---- What it is, and where ---------------------------------------
        Label {
            Layout.fillWidth: true
            text: model.summary + " · " + model.region
            font.family: Theme.textFamily
            font.pixelSize: Theme.captionSize
            opacity: 0.7
            elide: Label.ElideRight
        }

        // ---- The name it answers to, and when it was last seen ------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingLoose
            // A machine with no address yet has neither of these to show, and
            // an empty row of two blanks reads as something failing to load.
            // The test is the model's field rather than the sentence built
            // from it: `observed()` reads the clock, so asking it here would
            // re-evaluate this binding every second to get the same answer.
            visible: model.host !== "" || model.observed

            Label {
                text: model.host
                font.family: "monospace"
                font.pixelSize: Theme.captionSize
                opacity: 0.7
                elide: Label.ElideRight
                Layout.fillWidth: true
            }

            Label {
                text: card.observed()
                font.family: Theme.textFamily
                font.pixelSize: Theme.captionSize
                opacity: 0.5
                visible: text !== ""
            }
        }

        // ---- The ladder, while something is happening --------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingTight
            spacing: 0
            visible: card.launching

            Rectangle {
                Layout.fillWidth: true
                Layout.bottomMargin: Theme.spacingLoose
                implicitHeight: 1
                color: Theme.strokeCard
            }

            Repeater {
                id: ladderSteps
                // Re-evaluated whenever anything the steps are built from
                // moves, which is the model reset every refresh.
                model: card.launching ? card.ladder() : []

                delegate: RowLayout {
                    id: step
                    Layout.fillWidth: true
                    spacing: Theme.spacingLoose

                    readonly property bool isDone: modelData.state === "done"
                    readonly property bool isActive: modelData.state === "active"
                    readonly property bool isFailed: modelData.state === "failed"
                    readonly property bool isUnknown: modelData.state === "unknown"

                    // The marker, and the line that ties it to the next one.
                    // Not aligned to the top: the column has to stretch to the
                    // row's height or the connector has nothing to fill.
                    ColumnLayout {
                        spacing: 0

                        Rectangle {
                            // One body line tall, so a marker and its label sit
                            // on the same baseline without a magic number.
                            implicitWidth: Theme.bodyLineHeight
                            implicitHeight: Theme.bodyLineHeight
                            radius: Theme.bodyLineHeight / 2
                            color: step.isDone ? Theme.fillSuccess
                                               : step.isFailed ? Theme.fillCritical
                                                               : "transparent"
                            border.width: (step.isDone || step.isFailed) ? 0 : 1
                            border.color: step.isActive ? Theme.accent : Theme.fillNeutral

                            Label {
                                anchors.centerIn: parent
                                font.family: step.isUnknown ? Theme.textFamily : Theme.iconFamily
                                font.pixelSize: Theme.captionSize
                                // A question mark rather than a dashed ring:
                                // QML cannot dash a border, and the character
                                // is the part a person reads anyway.
                                text: step.isDone ? Theme.icon.accept
                                                  : step.isFailed ? Theme.icon.warning
                                                                  : step.isUnknown ? "?" : ""
                                color: (step.isDone || step.isFailed) ? Theme.fillCard
                                                                      : Theme.fillNeutral
                            }

                            // The one moving thing in the ladder, and it stops
                            // when the step resolves. Under reduced motion the
                            // duration collapses and it becomes a static dot,
                            // which still says "this is the step" — the pulse
                            // was the escort, not the message.
                            Rectangle {
                                anchors.centerIn: parent
                                visible: step.isActive
                                implicitWidth: Theme.spacingTight + 2
                                implicitHeight: Theme.spacingTight + 2
                                radius: width / 2
                                color: Theme.accent

                                SequentialAnimation on opacity {
                                    running: step.isActive && Theme.motion
                                    loops: Animation.Infinite
                                    NumberAnimation { to: 0.3; duration: Theme.durationSlow }
                                    NumberAnimation { to: 1.0; duration: Theme.durationSlow }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillHeight: true
                            Layout.alignment: Qt.AlignHCenter
                            implicitWidth: 1
                            visible: index < ladderSteps.count - 1
                            color: step.isDone ? Theme.fillSuccess : Theme.strokeCard
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.bottomMargin: index < ladderSteps.count - 1 ? Theme.spacing : 0
                        spacing: 0

                        Label {
                            Layout.fillWidth: true
                            text: modelData.label
                            font.family: Theme.textFamily
                            font.pixelSize: Theme.bodySize
                            font.weight: (step.isActive || step.isFailed) ? Theme.strongWeight
                                                                          : Theme.regularWeight
                            opacity: (step.isActive || step.isFailed) ? 1.0 : step.isDone ? 0.7 : 0.5
                            elide: Label.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            // The agent's own words when it said any, and the
                            // word "not observed" when nobody looked. Colour is
                            // never the only carrier of either.
                            text: modelData.note !== "" ? modelData.note
                                                        : step.isUnknown ? qsTr("not observed") : ""
                            visible: text !== ""
                            font.family: Theme.textFamily
                            font.pixelSize: Theme.captionSize
                            opacity: 0.6
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // How long, and how many times — shown only when Core has an
            // operation to date them from.
            //
            // The console's footer carries a third thing, `waiting_on`, and
            // this one does not: it is already the Booting step's note, where
            // it says which step is waiting as well as what for. Printing it
            // twice on a card this narrow would cost a line and add nothing.
            // `observed n ago` is not here either — it is up beside the
            // machine's name, because it is true of the machine rather than of
            // the operation, and the card has a permanent place for it.
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacingLoose
                spacing: Theme.spacingLoose
                visible: model.operating

                Label {
                    text: model.operating ? card.elapsed() : ""
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.6
                }

                Label {
                    // Attempt 1 is not news; attempt 4 is.
                    text: qsTr("attempt %1").arg(model.attempt)
                    visible: model.attempt > 1
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.6
                }
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
        }

        // ---- One primary action, and the rest under a menu ----------------
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingTight
            spacing: Theme.spacing

            Item { Layout.fillWidth: true }

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
                text: Theme.icon.more
                font.family: Theme.iconFamily
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
