// Omnuv: the top of the estate — which project, which version of it, and a
// count of what its machines are doing. The machine grid follows it and the
// rest of the estate is `EstateRail.qml`. What is waiting for capacity is
// drawn in the grid itself, and a refresh that failed is one notice under the
// top bar (`OmnuvView.qml`), which reads `stale` from here.
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

    // The reads that could not be refreshed. Their rows stay on screen,
    // dimmed where they are drawn; the view says why, once.
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
            text: Omnuv.projectName
            elide: Label.ElideRight
            Layout.maximumWidth: header.width * 0.6
            font.family: Theme.textFamily
            font.pixelSize: Theme.titleSize
            font.weight: Theme.strongWeight
        }

        // The version, as a thing with a name rather than a number in prose.
        Pill {
            visible: header.head !== null
            tone: Theme.accent
            strong: true
            text: header.head === null ? "" : qsTr("version %1").arg(header.head.epoch)
        }

        Label {
            Layout.fillWidth: true
            visible: header.head !== null
            text: header.head === null ? "" : qsTr("changed %1").arg(Estate.ago(header.head.at, header.now))
            elide: Label.ElideRight
            font.family: Theme.textFamily
            font.pixelSize: Theme.captionSize
            opacity: 0.6
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

    component Counter: Rectangle {
        id: counter
        property string label
        property string word
        readonly property var row: header.pulseRow(label)
        readonly property int n: row !== null ? row.n : 0
        // Running is green, starting amber, waiting the accent, stopped grey:
        // the same dot-and-word rule as every card, on the same wash as a pill.
        readonly property color tone: label === "running" ? Theme.fillSuccess
                                    : label === "starting" ? Theme.fillCaution
                                    : label === "waiting" ? Theme.accent
                                    : Theme.fillNeutral
        visible: row !== null
        implicitHeight: 28
        implicitWidth: counterRow.implicitWidth + 2 * Theme.spacingLoose
        radius: height / 2
        color: Qt.rgba(tone.r, tone.g, tone.b, 0.14)
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("%1 %2").arg(n).arg(word)

        Row {
            id: counterRow
            anchors.centerIn: parent
            spacing: Theme.spacing

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.spacing
                height: Theme.spacing
                radius: width / 2
                color: counter.label === "stopped" ? "transparent" : counter.tone
                border.width: counter.label === "stopped" ? 1 : 0
                border.color: counter.tone
            }

            Label {
                id: number
                anchors.verticalCenter: parent.verticalCenter
                text: counter.n
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                font.weight: Theme.strongWeight
                transformOrigin: Item.Center

                // A counter that moved pops once, alone. Nothing pulses at
                // rest, and with animations off this is instant.
                SequentialAnimation {
                    id: pop
                    NumberAnimation { target: number; property: "scale"; to: 1.25; duration: Theme.durationFaster }
                    NumberAnimation { target: number; property: "scale"; to: 1.0; duration: Theme.durationFast }
                }
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: counter.word
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                opacity: 0.8
            }
        }

        onNChanged: pop.restart()
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        visible: header.pulseRows.length > 0

        Counter { label: "running"; word: qsTr("running") }
        Counter { label: "starting"; word: qsTr("starting") }
        Counter { label: "waiting"; word: qsTr("waiting") }
        Counter { label: "stopped"; word: qsTr("stopped") }
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
