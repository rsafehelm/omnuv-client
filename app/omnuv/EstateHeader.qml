// Omnuv: the top of band two — which project, which version of it, and a
// count of what its machines are doing. The machine cards follow it; the rest
// of the band is `EstateFooter.qml`.
//
// **The version is the map's epoch.** "Version 41 of your cloud, and it last
// changed two minutes ago" is what the map exists to let a tenant say, so it
// is drawn from the newest history row and from nothing else — and not drawn
// at all until that row has arrived, because version 0 is a claim.

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import Omnuv 1.0
import "estate.js" as Estate

ColumnLayout {
    id: header
    spacing: Theme.spacingLoose

    // Milliseconds, ticked by the view, so "2m ago" moves on its own.
    property double now: Date.now()

    readonly property var estate: Omnuv.estate
    readonly property var historyRows: estate.history.data
    readonly property var head: historyRows !== undefined && historyRows.length > 0 ? historyRows[0] : null
    readonly property var parked: estate.parked.data
    readonly property var waiting: parked !== undefined ? Estate.waiting(parked) : []

    // One banner for the whole band. Rows that could not be refreshed stay on
    // screen, dimmed by their own column; this says why, once.
    readonly property var stale: Estate.staleOf([estate.parked, estate.networks, estate.devices,
                                                 estate.endpoints, estate.keys, estate.usage,
                                                 estate.history])

    function oldestStale() {
        var at = null
        for (var i = 0; i < stale.length; i++) {
            if (at === null || stale[i].readAt < at) {
                at = stale[i].readAt
            }
        }
        return at
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacingLoose

        Label {
            Layout.fillWidth: true
            text: Omnuv.projectName
            elide: Label.ElideRight
            font.family: Theme.textFamily
            font.pixelSize: Theme.subtitleSize
            font.weight: Theme.strongWeight
        }

        Label {
            visible: header.head !== null
            text: header.head === null ? ""
                                       : qsTr("version %1 · last change %2")
                                         .arg(header.head.epoch)
                                         .arg(Estate.ago(header.head.at, header.now))
            font.family: Theme.textFamily
            font.pixelSize: Theme.captionSize
            opacity: 0.7
        }
    }

    // The pulse. Which counters exist, and their numbers, come from
    // `Estate.pulse()`: a counter whose source was not read is absent, never
    // 0. Four fixed items rather than a Repeater over that list, because a
    // model rebuilt every refresh recreates its delegates, and a counter that
    // moved could then never pop alone.
    readonly property var pulseRows: Estate.pulse(Omnuv.machines.statusCounts, Omnuv.machines.loaded, parked)

    function pulseRow(label) {
        for (var i = 0; i < pulseRows.length; i++) {
            if (pulseRows[i].label === label) {
                return pulseRows[i]
            }
        }
        return null
    }

    component Counter: Row {
        id: counter
        property string label
        property string word
        readonly property var row: header.pulseRow(label)
        readonly property int n: row !== null ? row.n : 0
        visible: row !== null
        spacing: Theme.spacingTight
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("%1 %2").arg(n).arg(word)

        // Running fills, starting is drawn as progress, waiting is a ring in
        // the accent and stopped is hollow: the dot-and-word rule of every card.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.spacing
            height: Theme.spacing
            radius: width / 2
            color: counter.label === "running" ? Theme.fillSuccess
                 : counter.label === "starting" ? Theme.fillCaution
                 : "transparent"
            border.width: counter.label === "stopped" || counter.label === "waiting" ? 1 : 0
            border.color: counter.label === "waiting" ? Theme.accent : Theme.fillNeutral
        }

        Label {
            id: number
            text: counter.n
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            font.weight: Theme.strongWeight
            transformOrigin: Item.Center

            // A counter that moved pops once, alone. Nothing pulses at rest,
            // and with animations off this is instant.
            SequentialAnimation {
                id: pop
                NumberAnimation { target: number; property: "scale"; to: 1.25; duration: Theme.durationFaster }
                NumberAnimation { target: number; property: "scale"; to: 1.0; duration: Theme.durationFast }
            }
        }

        Label {
            text: counter.word
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            opacity: 0.7
        }

        onNChanged: pop.restart()
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.padding
        visible: header.pulseRows.length > 0

        Counter { label: "running"; word: qsTr("running") }
        Counter { label: "starting"; word: qsTr("starting") }
        Counter { label: "waiting"; word: qsTr("waiting") }
        Counter { label: "stopped"; word: qsTr("stopped") }
    }

    // Stale: what is shown is kept, and said to be old.
    Rectangle {
        Layout.fillWidth: true
        visible: header.stale.length > 0
        implicitHeight: staleRow.implicitHeight + 2 * Theme.spacing
        radius: Theme.radiusControl
        color: "transparent"
        border.width: 1
        border.color: Theme.fillCaution

        RowLayout {
            id: staleRow
            anchors.fill: parent
            anchors.margins: Theme.spacing
            spacing: Theme.spacing

            Label {
                Layout.fillWidth: true
                text: header.stale.length === 0 ? ""
                      : qsTr("Showing %1 — could not refresh: %2")
                        .arg(Qt.formatTime(header.oldestStale(), "HH:mm"))
                        .arg(header.stale[0].problem)
                wrapMode: Text.WordWrap
                font.family: Theme.textFamily
                font.pixelSize: Theme.captionSize
            }

            Button {
                text: qsTr("Try again")
                flat: true
                onClicked: Omnuv.estate.retryAll()
            }
        }
    }

    // Waiting requests that could not be read at all. The pulse leaves the
    // counter out, which is honest and also silent; this says why.
    Label {
        Layout.fillWidth: true
        visible: header.estate.parked.state === "unavailable"
        text: qsTr("Waiting requests could not be read: %1").arg(header.estate.parked.problem)
        wrapMode: Text.WordWrap
        font.family: Theme.textFamily
        font.pixelSize: Theme.captionSize
        color: Theme.fillCritical
    }

    // Requests waiting for capacity. A clock, not a spinner: waiting is not
    // activity, and the reason is Core's, verbatim.
    Repeater {
        model: header.waiting

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Label {
                Layout.fillWidth: true
                text: qsTr("Waiting: %1").arg(modelData.waiting_on)
                wrapMode: Text.WordWrap
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("%1 · gives up %2").arg(modelData.billing)
                                                  .arg(Estate.ago(modelData.expires_at, header.now))
                wrapMode: Text.WordWrap
                font.family: Theme.textFamily
                font.pixelSize: Theme.captionSize
                opacity: 0.7
            }
        }
    }

    // Nothing rented yet — said only once the list has actually been read.
    Label {
        Layout.fillWidth: true
        Layout.topMargin: Theme.padding
        visible: Omnuv.machines.loaded && Omnuv.machines.count === 0 && header.waiting.length === 0
        text: qsTr("No machines yet. Rent one in the Omnuv console and it will appear here.")
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        font.family: Theme.textFamily
        font.pixelSize: Theme.bodySize
        opacity: 0.7
    }
}
