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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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
            // A number, not a share of the header's width: a limit that
            // follows the width the layout is in the middle of deciding sends
            // the layout round again, and Qt logs the loop.
            Layout.maximumWidth: 520
            font.family: Theme.displayFamily
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

    // Running is green, starting amber, waiting the accent, stopped grey: the
    // same colours, under the same words, as every card.
    function tone(label) {
        return label === "running" ? Theme.fillSuccess
             : label === "starting" ? Theme.fillCaution
             : label === "waiting" ? Theme.accent
             : label === "attention" ? Theme.fillCritical
             : Theme.fillNeutral
    }

    // **The fleet at a glance: one thin bar, shared out by what the machines
    // are doing.** Six machines running and one stopped is a bar that is
    // nearly all green before a number is read, which four equal pills could
    // never say. The legend beneath carries the numbers and the words, so the
    // bar is never the only carrier of anything.
    // What the bar is shared between: the counters that have something to
    // show. Widths are worked out here rather than by a layout, because a
    // layout inside a layout whose children come and go rearranges itself in
    // a loop, and says so in the log.
    readonly property var meterRows: {
        var out = []
        for (var i = 0; i < pulseRows.length; i++) {
            if (pulseRows[i].n > 0) {
                out.push(pulseRows[i])
            }
        }
        return out
    }
    readonly property int meterTotal: {
        var n = 0
        for (var i = 0; i < meterRows.length; i++) {
            n += meterRows[i].n
        }
        return n
    }

    Item {
        id: meter
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        implicitHeight: 6
        // **Not under high contrast**, where every fill collapses to one
        // colour: five segments of the same white read as one long bar, which
        // says something untrue. The legend below carries every number and
        // every word, which is why the bar can simply go.
        visible: header.meterTotal > 0 && !Theme.highContrast
        Accessible.ignored: true

        Row {
            spacing: 3

            Repeater {
                model: header.meterRows

                Rectangle {
                    width: Math.max(6, (meter.width - 3 * (header.meterRows.length - 1)) * modelData.n / header.meterTotal)
                    height: 6
                    radius: 3
                    color: header.tone(modelData.label)
                    opacity: modelData.label === "stopped" ? 0.45 : 1
                }
            }
        }
    }

    component Counter: Row {
        id: counter
        property string label
        property string word
        readonly property var row: header.pulseRow(label)
        readonly property int n: row !== null ? row.n : 0
        visible: row !== null
        spacing: Theme.spacingTight + 2
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("%1 %2").arg(n).arg(word)

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.spacing
            height: Theme.spacing
            radius: width / 2
            color: counter.label === "stopped" ? "transparent" : header.tone(counter.label)
            border.width: counter.label === "stopped" ? 1 : 0
            border.color: header.tone(counter.label)
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
            opacity: 0.78
        }

        onNChanged: pop.restart()
    }

    // A Row, not a Flow: a Flow's height follows its width, and inside a
    // ColumnLayout that is a layout rearranging itself mid-rearrange — Qt says
    // so in the log and gives up after two turns. Five short counters fit the
    // narrowest window this view is laid out for.
    Row {
        spacing: Theme.padding
        // Nothing to count is said once, by the empty state in the grid —
        // not four times over as a row of zeros.
        visible: header.meterTotal > 0

        Counter { label: "running"; word: qsTr("running") }
        Counter { label: "starting"; word: qsTr("starting") }
        Counter { label: "attention"; word: counter_attention.n === 1 ? qsTr("needs attention") : qsTr("need attention"); id: counter_attention }
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
}
