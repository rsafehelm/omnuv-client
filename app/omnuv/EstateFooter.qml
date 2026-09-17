// Omnuv: the rest of band two — network, endpoints, inference and spend —
// and band three, the map's history collapsed to its last three changes.
//
// Every column draws its read's state, and the four states look different on
// purpose: a first load is one dim line, a failed first read is its own
// sentence with Try again, a failed refresh keeps the rows and dims them
// (the header says why, once), and only a current answer is drawn plainly.
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
    id: footer
    spacing: Theme.spacingLoose

    property double now: Date.now()

    readonly property var estate: Omnuv.estate

    // A card with a title whose body follows one read's state.
    component Tile: Rectangle {
        id: tile
        property string title
        property var read: null
        default property alias content: body.data
        readonly property string state_: read ? read.state : "current"

        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop
        implicitHeight: column.implicitHeight + 2 * Theme.padding
        radius: Theme.radiusControl
        color: Theme.fillCard
        border.width: 1
        border.color: Theme.strokeCard

        ColumnLayout {
            id: column
            x: Theme.padding
            y: Theme.padding
            width: tile.width - 2 * Theme.padding
            spacing: Theme.spacingTight

            Label {
                Layout.fillWidth: true
                text: tile.title
                font.family: Theme.textFamily
                font.pixelSize: Theme.captionSize
                font.weight: Theme.strongWeight
                opacity: 0.7
            }

            Label {
                visible: tile.state_ === "loading"
                text: qsTr("Reading…")
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                opacity: 0.4
            }

            Label {
                Layout.fillWidth: true
                visible: tile.state_ === "unavailable"
                text: tile.read ? tile.read.problem : ""
                wrapMode: Text.WordWrap
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                color: Theme.fillCritical
            }

            Button {
                visible: tile.state_ === "unavailable"
                text: qsTr("Try again")
                flat: true
                onClicked: tile.read.retry()
            }

            ColumnLayout {
                id: body
                Layout.fillWidth: true
                spacing: Theme.spacingTight
                visible: tile.state_ === "current" || tile.state_ === "stale"
                opacity: tile.state_ === "stale" ? 0.6 : 1
            }
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
        opacity: 0.7
    }

    // ------------------------------------------------------------ band two

    GridLayout {
        Layout.fillWidth: true
        columns: footer.width >= 720 ? 3 : 1
        columnSpacing: Theme.spacingLoose
        rowSpacing: Theme.spacingLoose

        Tile {
            id: networkTile
            title: qsTr("NETWORK")
            read: footer.estate.networks
            readonly property var net: {
                var d = footer.estate.networks.data
                return d !== undefined && d.length > 0 ? d[0] : null
            }
            readonly property var devices: footer.estate.devices.data

            Line {
                visible: networkTile.net === null
                text: qsTr("No private network yet")
                opacity: 0.7
            }
            Line {
                visible: networkTile.net !== null
                text: networkTile.net ? networkTile.net.name : ""
                font.weight: Theme.strongWeight
            }
            Caption {
                visible: networkTile.net !== null
                text: networkTile.net === null ? ""
                      : qsTr("%1 · %2 addresses").arg(networkTile.net.cidr)
                                                     .arg(networkTile.net.addresses.length)
            }

            // Devices on the Client VPN: a dot and a name. Connected fills,
            // pending is a ring in the accent, offline is hollow.
            Caption {
                visible: networkTile.net !== null && footer.estate.devices.state === "unavailable"
                text: qsTr("Devices could not be read: %1").arg(footer.estate.devices.problem)
            }
            Caption {
                visible: networkTile.devices !== undefined && networkTile.devices.length === 0
                text: qsTr("No devices have joined")
            }
            Repeater {
                model: networkTile.devices !== undefined ? networkTile.devices : []

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing
                    opacity: footer.estate.devices.state === "stale" ? 0.6 : 1

                    Rectangle {
                        width: Theme.spacing
                        height: Theme.spacing
                        radius: width / 2
                        color: modelData.status === "Connected" ? Theme.fillSuccess : "transparent"
                        border.width: modelData.status === "Connected" ? 0 : 1
                        border.color: modelData.status === "Pending" ? Theme.accent : Theme.fillNeutral
                    }
                    Line {
                        text: modelData.name
                    }
                    Label {
                        text: modelData.status === "Connected" ? qsTr("connected")
                            : modelData.status === "Pending" ? qsTr("waiting to join")
                            : qsTr("last seen %1").arg(Estate.ago(modelData.last_seen_at, footer.now))
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.captionSize
                        opacity: 0.7
                    }
                }
            }
        }

        Tile {
            id: endpointTile
            title: qsTr("ENDPOINTS")
            read: footer.estate.endpoints
            readonly property var rows: footer.estate.endpoints.data

            Line {
                visible: endpointTile.rows !== undefined && endpointTile.rows.length === 0
                text: qsTr("Nothing is published")
                opacity: 0.7
            }
            Repeater {
                model: endpointTile.rows !== undefined ? endpointTile.rows : []

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing

                        Rectangle {
                            width: Theme.spacing
                            height: Theme.spacing
                            radius: width / 2
                            color: modelData.status === "Active" ? Theme.fillSuccess
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

        Tile {
            id: inferenceTile
            title: qsTr("INFERENCE")
            read: footer.estate.keys
            readonly property var keys: footer.estate.keys.data
            readonly property var usage: footer.estate.usage.data

            Line {
                text: inferenceTile.keys === undefined ? ""
                      : inferenceTile.keys.length === 0 ? qsTr("No API keys yet")
                      : inferenceTile.keys.length === 1 ? qsTr("1 API key")
                      : qsTr("%1 API keys").arg(inferenceTile.keys.length)
            }
            Caption {
                visible: inferenceTile.usage !== undefined
                text: inferenceTile.usage === undefined ? ""
                      : qsTr("%1 requests in 30 days")
                        .arg(Number(inferenceTile.usage.requests).toLocaleString(Qt.locale(), "f", 0))
            }
        }
    }

    // Spend. An estimate, said as one: while Omnuv is in closed testing
    // nothing is charged, and no surface may imply otherwise.
    Tile {
        id: spendTile
        title: qsTr("SPEND, 30 DAYS · ESTIMATE")
        read: footer.estate.usage
        readonly property var usage: footer.estate.usage.data
        // The day, not the second: the series changes when a day turns over
        // or an answer arrives, and a binding on the ticking clock would
        // recompute — and redraw — thirty points every second.
        readonly property double day: Math.floor(footer.now / 86400000) * 86400000
        readonly property var points: Estate.series(usage, day)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingLoose

            // Drawn when the numbers change, and only then animated: the same
            // thirty days arriving again on the next poll repaint nothing.
            // A day with nothing spent is a zero, not a gap, which is what
            // `Estate.series` returns.
            Canvas {
                id: spark
                Layout.preferredWidth: 160
                Layout.preferredHeight: 28
                property var points: spendTile.points
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
                    if (!p || p.length < 2) {
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

            Label {
                text: spendTile.usage ? Estate.money(spendTile.usage.cost, spendTile.usage.currency) : ""
                font.family: Theme.textFamily
                font.pixelSize: Theme.subtitleSize
                font.weight: Theme.strongWeight
            }

            Caption {
                text: spendTile.usage === undefined ? ""
                      : qsTr("compute %1 · inference %2")
                        .arg(Estate.money(spendTile.usage.compute_cost, spendTile.usage.currency))
                        .arg(Estate.money(spendTile.usage.inference_cost, spendTile.usage.currency))
            }
        }
    }

    // ---------------------------------------------------------- band three

    Tile {
        id: historyTile
        title: qsTr("WHAT CHANGED")
        read: footer.estate.history
        readonly property var rows: footer.estate.history.data
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
            visible: historyTile.broken.length > 0
            text: qsTr("The record does not follow from the version before at %1. Tell Omnuv support.")
                  .arg(historyTile.broken.join(", "))
            color: Theme.fillCritical
            opacity: 1
        }
        Line {
            visible: historyTile.rows !== undefined && historyTile.rows.length === 0
            text: qsTr("No changes recorded yet")
            opacity: 0.7
        }
        Repeater {
            model: historyTile.rows !== undefined ? historyTile.rows : []

            GridLayout {
                Layout.fillWidth: true
                columns: 3
                columnSpacing: Theme.spacing
                rowSpacing: 0

                Label {
                    Layout.preferredWidth: 72
                    text: qsTr("version %1").arg(modelData.epoch)
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.7
                }
                Label {
                    Layout.preferredWidth: 150
                    text: Estate.ago(modelData.at, footer.now) + " · " + modelData.actor
                    elide: Label.ElideRight
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.7
                }
                Label {
                    Layout.fillWidth: true
                    text: Estate.sentence(modelData)
                    wrapMode: Text.WordWrap
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                }
            }
        }
        Caption {
            visible: historyTile.rows !== undefined && historyTile.rows.length > 0
            text: qsTr("Earlier changes are in the Omnuv console.")
        }
    }
}
