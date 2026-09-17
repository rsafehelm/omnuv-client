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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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
        property string icon: ""
        // A word beside the title: "Estimate".
        property string tag: ""
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
            color: Theme.strokeDivider
        }

        // A glyph and a name in sentence case — Windows does not shout its
        // headings — with whatever the section wants to say beside them.
        RowLayout {
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.spacingTight
            spacing: Theme.spacing

            Glyph {
                icon: section.icon
                size: 14
                opacity: 0.7
            }
            Label {
                text: section.title
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                font.weight: Theme.strongWeight
            }
            Pill {
                visible: section.tag !== ""
                text: section.tag
            }
            Item {
                Layout.fillWidth: true
            }
        }

        // A first load is the shape of what is coming, breathing — not a
        // word. It stops the moment the read lands or fails, and under
        // reduced motion it holds still.
        Repeater {
            model: section.state_ === "loading" ? [0.62, 0.38] : []

            Rectangle {
                Layout.preferredWidth: section.width * modelData
                implicitHeight: index === 0 ? 14 : 10
                Layout.topMargin: 2
                radius: Theme.radiusControl
                color: Theme.fillSubtle
                Accessible.name: qsTr("Reading")

                SequentialAnimation on opacity {
                    // Only while it can be seen: an animation on a hidden
                    // item still ticks, and an organization with no project
                    // hides this rail with its reads never made.
                    running: Theme.motion && parent.visible
                    loops: Animation.Infinite
                    NumberAnimation { from: 1; to: 0.35; duration: 700; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.35; to: 1; duration: 700; easing.type: Easing.InOutSine }
                }
            }
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
        title: qsTr("Network")
        icon: Theme.icon.network
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
        title: qsTr("Endpoints")
        icon: Theme.icon.globe
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
        title: qsTr("Inference")
        icon: Theme.icon.key
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
        title: qsTr("Spend, 30 days")
        icon: Theme.icon.spend
        tag: qsTr("Estimate")
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
                font.family: Theme.displayFamily
                font.pixelSize: Theme.titleSize
                font.weight: Theme.strongWeight
            }

            // Drawn when the numbers change, and only then animated: the same
            // thirty days arriving again on the next poll repaint nothing. A
            // day with nothing spent is a zero, not a gap.
            Canvas {
                id: spark
                Layout.fillWidth: true
                Layout.preferredHeight: 36
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
                    var a = Theme.accent
                    var rgb = Math.round(a.r * 255) + "," + Math.round(a.g * 255) + "," + Math.round(a.b * 255)
                    var x = 0, y = 0
                    ctx.beginPath()
                    for (var j = 0; j <= last; j++) {
                        x = j * (width - 8) / (p.length - 1) + 1
                        // The floor sits a little above the picture's edge,
                        // so a quiet month still has an area under it.
                        y = height - 12 - (max > 0 ? p[j] / max * (height - 18) : 0)
                        if (j === 0) {
                            ctx.moveTo(x, y)
                        }
                        else {
                            ctx.lineTo(x, y)
                        }
                    }
                    ctx.lineJoin = "round"
                    ctx.lineWidth = 2
                    ctx.strokeStyle = "rgba(" + rgb + ",1)"
                    ctx.stroke()
                    // The area under it, fading out: what Dev Home's widgets
                    // and Task Manager do, and what makes a line read as an
                    // amount rather than as a scribble.
                    ctx.lineTo(x, height)
                    ctx.lineTo(1, height)
                    ctx.closePath()
                    var fill = ctx.createLinearGradient(0, 0, 0, height)
                    fill.addColorStop(0, "rgba(" + rgb + ",0.38)")
                    fill.addColorStop(1, "rgba(" + rgb + ",0)")
                    ctx.fillStyle = fill
                    ctx.fill()
                    // Today.
                    ctx.beginPath()
                    ctx.arc(x, y, 3, 0, Math.PI * 2)
                    ctx.fillStyle = "rgba(" + rgb + ",1)"
                    ctx.fill()
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
        title: qsTr("Changes")
        icon: Theme.icon.history
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

            // A timeline: a dot per change and a thread between them, so
            // three changes read as one history rather than three notes.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingLoose

                Item {
                    Layout.fillHeight: true
                    implicitWidth: 8

                    Rectangle {
                        visible: index < changes.rows.length - 1
                        x: 3.5
                        y: 14
                        width: 1
                        height: parent.height - 8
                        color: Theme.strokeDivider
                    }
                    Rectangle {
                        y: 6
                        width: 8
                        height: 8
                        radius: 4
                        color: index === 0 ? Theme.accent : "transparent"
                        border.width: index === 0 ? 0 : 1
                        border.color: Theme.fillNeutral
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.bottomMargin: Theme.spacingLoose
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
        }
        Caption {
            visible: changes.rows !== undefined && changes.rows.length > 0
            text: qsTr("Earlier changes are in the Omnuv console.")
        }
    }
}
