// Omnuv: the estate beside the machines — network, endpoints, inference,
// spend, and the map's last three changes — as plain text on the window's own
// surface. No frames: the machines are the only cards on this screen, because
// they are the only things a person acts on, and a column of boxes made every
// piece look like a dialog of its own.
//
// Every section follows its read's state, and the four look different on
// purpose: a first load is one dim line, a failed first read is Core's
// sentence with Try again, a failed refresh keeps the rows and dims them (the
// view's notice says why, once), and only a current answer is drawn plainly.
// Nothing here turns an unread list into "none".
//
// **No provider, in any form.** An endpoint shows the address the buyer uses,
// never the target behind it; the history names *Omnuv* for marketplace
// changes, which is Core's resolution, never the provider that observed them.

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import Omnuv 1.0
import "estate.js" as Estate

ColumnLayout {
    id: rail
    spacing: Theme.padding

    property double now: Date.now()

    readonly property var estate: Omnuv.estate

    // A heading, and a body that follows one read's state.
    component Section: ColumnLayout {
        id: section
        property string title
        property var read: null
        // A hairline above every section but the first: sections are told
        // apart by space and a rule, never by a box.
        property bool rule: true
        default property alias content: body.data
        readonly property string state_: read ? read.state : "current"

        Layout.fillWidth: true
        spacing: Theme.spacingTight

        Rectangle {
            visible: section.rule
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.spacingLoose
            implicitHeight: 1
            color: Theme.strokeCard
        }

        Label {
            Layout.fillWidth: true
            text: section.title
            font.family: Theme.textFamily
            font.pixelSize: Theme.captionSize
            font.weight: Theme.strongWeight
            font.letterSpacing: 0.5
            opacity: 0.6
        }

        Label {
            visible: section.state_ === "loading"
            text: qsTr("Reading…")
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            opacity: 0.4
        }

        Label {
            Layout.fillWidth: true
            visible: section.state_ === "unavailable"
            text: section.read ? section.read.problem : ""
            wrapMode: Text.WordWrap
            font.family: Theme.textFamily
            font.pixelSize: Theme.captionSize
            color: Theme.fillCritical
        }

        Button {
            visible: section.state_ === "unavailable"
            text: qsTr("Try again")
            flat: true
            leftPadding: 0
            onClicked: section.read.retry()
        }

        ColumnLayout {
            id: body
            Layout.fillWidth: true
            spacing: Theme.spacingTight
            visible: section.state_ === "current" || section.state_ === "stale"
            opacity: section.state_ === "stale" ? 0.6 : 1
        }
    }

    component Line: Label {
        Layout.fillWidth: true
        elide: Label.ElideRight
        font.family: Theme.textFamily
        font.pixelSize: Theme.bodySize
    }

    component Caption: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.family: Theme.textFamily
        font.pixelSize: Theme.captionSize
        opacity: 0.6
    }

    component Dot: Rectangle {
        property bool filled: false
        property color tone: Theme.fillNeutral
        Layout.alignment: Qt.AlignVCenter
        implicitWidth: Theme.spacing
        implicitHeight: Theme.spacing
        radius: width / 2
        color: filled ? tone : "transparent"
        border.width: filled ? 0 : 1
        border.color: tone
    }

    // ------------------------------------------------------------- network

    Section {
        id: network
        rule: false
        title: qsTr("NETWORK")
        read: rail.estate.networks
        readonly property var net: {
            var d = rail.estate.networks.data
            return d !== undefined && d.length > 0 ? d[0] : null
        }
        readonly property var devices: rail.estate.devices.data

        Line {
            visible: network.net === null
            text: qsTr("No private network yet")
            opacity: 0.6
        }
        Line {
            visible: network.net !== null
            text: network.net ? network.net.name : ""
            font.weight: Theme.strongWeight
        }
        Caption {
            visible: network.net !== null
            text: network.net === null ? ""
                  : qsTr("%1 · %2 addresses").arg(network.net.cidr).arg(network.net.addresses.length)
        }
        Caption {
            visible: network.net !== null && rail.estate.devices.state === "unavailable"
            text: qsTr("Devices could not be read: %1").arg(rail.estate.devices.problem)
            color: Theme.fillCritical
            opacity: 1
        }
        Caption {
            visible: network.devices !== undefined && network.devices.length === 0
            text: qsTr("No devices have joined")
        }

        // Devices on the Client VPN: connected fills, waiting to join is a ring
        // in the accent, offline is hollow — and the word is always beside it.
        Repeater {
            model: network.devices !== undefined ? network.devices : []

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: index === 0 ? Theme.spacingTight : 0
                spacing: Theme.spacing
                opacity: rail.estate.devices.state === "stale" ? 0.6 : 1

                Dot {
                    filled: modelData.status === "Connected"
                    tone: modelData.status === "Connected" ? Theme.fillSuccess
                        : modelData.status === "Pending" ? Theme.accent
                        : Theme.fillNeutral
                }
                Line {
                    text: modelData.name
                }
                Label {
                    text: modelData.status === "Connected" ? qsTr("connected")
                        : modelData.status === "Pending" ? qsTr("waiting to join")
                        : Estate.ago(modelData.last_seen_at, rail.now)
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.6
                }
            }
        }
    }

    // ----------------------------------------------------------- endpoints

    Section {
        id: endpoints
        title: qsTr("ENDPOINTS")
        read: rail.estate.endpoints
        readonly property var rows: rail.estate.endpoints.data

        Line {
            visible: endpoints.rows !== undefined && endpoints.rows.length === 0
            text: qsTr("Nothing is published")
            opacity: 0.6
        }
        Repeater {
            model: endpoints.rows !== undefined ? endpoints.rows : []

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Dot {
                        filled: modelData.status === "Active"
                        tone: modelData.status === "Active" ? Theme.fillSuccess
                            : modelData.suspended_reason ? Theme.fillCritical
                            : Theme.fillNeutral
                    }
                    Line {
                        text: modelData.url
                    }
                }
                Caption {
                    text: qsTr("%1 · %2 · %3 out").arg(modelData.status)
                                                          .arg(modelData.tier)
                                                          .arg(Estate.bytes(modelData.bytes_out))
                }
                Caption {
                    visible: !!modelData.suspended_reason
                    text: qsTr("Suspended: %1").arg(modelData.suspended_reason || "")
                    color: Theme.fillCritical
                    opacity: 1
                }
            }
        }
    }

    // ----------------------------------------------------------- inference

    Section {
        id: inference
        title: qsTr("INFERENCE")
        read: rail.estate.keys
        readonly property var keys: rail.estate.keys.data
        readonly property var usage: rail.estate.usage.data

        Line {
            text: inference.keys === undefined ? ""
                  : inference.keys.length === 0 ? qsTr("No API keys yet")
                  : inference.keys.length === 1 ? qsTr("1 API key")
                  : qsTr("%1 API keys").arg(inference.keys.length)
        }
        Caption {
            visible: inference.usage !== undefined
            text: inference.usage === undefined ? ""
                  : qsTr("%1 requests in 30 days")
                    .arg(Number(inference.usage.requests).toLocaleString(Qt.locale(), "f", 0))
        }
    }

    // --------------------------------------------------------------- spend

    // An estimate, said as one: while Omnuv is in closed testing nothing is
    // charged, and no surface may imply otherwise.
    Section {
        id: spend
        title: qsTr("SPEND, 30 DAYS · ESTIMATE")
        read: rail.estate.usage
        readonly property var usage: rail.estate.usage.data
        // The day, not the second: the series changes when a day turns over
        // or an answer arrives, and a binding on the ticking clock would
        // recompute — and redraw — thirty points every second.
        readonly property double day: Math.floor(rail.now / 86400000) * 86400000
        readonly property var points: Estate.series(usage, day)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                text: spend.usage ? Estate.money(spend.usage.cost, spend.usage.currency) : ""
                font.family: Theme.textFamily
                font.pixelSize: Theme.subtitleSize
                font.weight: Theme.strongWeight
            }

            // Drawn when the numbers change, and only then animated: the same
            // thirty days arriving again on the next poll repaint nothing. A
            // day with nothing spent is a zero, not a gap.
            Canvas {
                id: spark
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                Layout.alignment: Qt.AlignVCenter
                property var points: spend.points
                property string drawn: ""
                property real progress: 1
                onPointsChanged: {
                    var key = JSON.stringify(points)
                    if (key === drawn) {
                        return
                    }
                    drawn = key
                    progress = 0
                    draw.restart()
                }
                onProgressChanged: requestPaint()
                onWidthChanged: requestPaint()
                NumberAnimation on progress {
                    id: draw
                    running: false
                    from: 0
                    to: 1
                    duration: Theme.durationSlow
                }
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    var p = points
                    if (!p || p.length < 2 || width < 4) {
                        return
                    }
                    var max = 0
                    for (var i = 0; i < p.length; i++) {
                        max = Math.max(max, p[i])
                    }
                    var last = Math.max(1, Math.round((p.length - 1) * progress))
                    ctx.strokeStyle = Theme.accent
                    ctx.lineWidth = 1.5
                    ctx.beginPath()
                    for (var j = 0; j <= last; j++) {
                        var x = j * (width - 2) / (p.length - 1) + 1
                        var y = height - 2 - (max > 0 ? p[j] / max * (height - 4) : 0)
                        if (j === 0) {
                            ctx.moveTo(x, y)
                        }
                        else {
                            ctx.lineTo(x, y)
                        }
                    }
                    ctx.stroke()
                }
            }
        }
        Caption {
            text: spend.usage === undefined ? ""
                  : qsTr("compute %1 · inference %2")
                    .arg(Estate.money(spend.usage.compute_cost, spend.usage.currency))
                    .arg(Estate.money(spend.usage.inference_cost, spend.usage.currency))
        }
    }

    // ------------------------------------------------------------- changes

    Section {
        id: changes
        title: qsTr("CHANGES")
        read: rail.estate.history
        readonly property var rows: rail.estate.history.data
        readonly property var broken: {
            var out = []
            for (var i = 0; rows !== undefined && i < rows.length; i++) {
                if (!rows[i].chained) {
                    out.push(rows[i].epoch)
                }
            }
            return out
        }

        // A chain that does not follow is a finding, and it is said as one.
        Caption {
            visible: changes.broken.length > 0
            text: qsTr("The record does not follow from the version before at %1. Tell Omnuv support.")
                  .arg(changes.broken.join(", "))
            color: Theme.fillCritical
            opacity: 1
        }
        Line {
            visible: changes.rows !== undefined && changes.rows.length === 0
            text: qsTr("No changes recorded yet")
            opacity: 0.6
        }
        Repeater {
            model: changes.rows !== undefined ? changes.rows : []

            ColumnLayout {
                Layout.fillWidth: true
                Layout.bottomMargin: Theme.spacing
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    text: Estate.sentence(modelData)
                    wrapMode: Text.WordWrap
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                }
                Caption {
                    text: qsTr("%1 · %2 · version %3").arg(Estate.ago(modelData.at, rail.now))
                                                                .arg(modelData.actor)
                                                                .arg(modelData.epoch)
                    elide: Label.ElideRight
                    wrapMode: Text.NoWrap
                }
            }
        }
        Caption {
            visible: changes.rows !== undefined && changes.rows.length > 0
            text: qsTr("Earlier changes are in the Omnuv console.")
        }
    }
}
