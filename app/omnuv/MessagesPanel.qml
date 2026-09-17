// Omnuv: what Omnuv has to say, along the bottom of the window. Read-only.
//
// Folded, it is one line: the newest message and how long ago. Open, it is the
// recent list. The messages are Core's operational events for the project in
// view, in Core's own sentences; the event's `detail` is never drawn, because
// it is a record for the console and not a sentence for a person. What this
// device itself last reported (`Omnuv.status`) leads the list, marked as this
// device's, since it is the one message Core cannot know.
//
// Severity is a word and a tone, never a tone alone.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omnuv 1.0
import "estate.js" as Estate

Rectangle {
    id: panel

    property double now: Date.now()
    property bool open: false

    readonly property var read: Omnuv.estate.events
    readonly property var rows: read.data !== undefined ? read.data : []
    readonly property var newest: rows.length > 0 ? rows[0] : null
    readonly property int warnings: {
        var n = 0
        for (var i = 0; i < rows.length; i++) {
            if (rows[i].severity !== "info") {
                n++
            }
        }
        return n
    }

    function tone(severity) {
        return severity === "error" ? Theme.fillCritical
             : severity === "warn" ? Theme.fillCaution
             : Theme.fillNeutral
    }

    function word(severity) {
        return severity === "error" ? qsTr("problem")
             : severity === "warn" ? qsTr("warning")
             : qsTr("note")
    }

    // A quiet bar along the window's floor, on the layer fill — the surface
    // Windows puts content on above Mica — the way Docker Desktop and Visual
    // Studio keep a system line at the bottom. One step below a card, so it
    // never competes with the machines.
    implicitHeight: column.implicitHeight
    radius: Theme.radiusOverlay
    color: Theme.fillLayer
    border.width: 1
    border.color: Theme.strokeCard
    clip: true

    Behavior on implicitHeight {
        NumberAnimation {
            duration: Theme.durationNormal
            easing.type: Easing.Bezier
            easing.bezierCurve: Theme.easeEntrance
        }
    }

    ColumnLayout {
        id: column
        x: Theme.padding
        width: parent.width - 2 * Theme.padding
        spacing: 0

        // The fold: a title, the newest message when folded, and the toggle.
        ItemDelegate {
            id: fold
            Layout.fillWidth: true
            Layout.topMargin: 1
            implicitHeight: 40
            leftPadding: 0
            rightPadding: 0
            Accessible.name: panel.open ? qsTr("Hide messages from Omnuv") : qsTr("Show messages from Omnuv")
            onClicked: panel.open = !panel.open

            background: Item {}
            contentItem: RowLayout {
                spacing: Theme.spacingLoose

                Glyph {
                    icon: Theme.icon.message
                    size: 14
                    opacity: 0.7
                }
                Label {
                    text: qsTr("Messages from Omnuv")
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                    font.weight: Theme.strongWeight
                }

                Pill {
                    visible: panel.warnings > 0
                    tone: Theme.fillCaution
                    dot: true
                    text: panel.warnings === 1 ? qsTr("1 needs a look") : qsTr("%1 need a look").arg(panel.warnings)
                }

                // Folded: the newest thing said, so the panel is useful shut.
                Label {
                    Layout.fillWidth: true
                    visible: !panel.open
                    text: Omnuv.status !== "" ? Omnuv.status
                          : panel.read.state === "loading" ? qsTr("Reading…")
                          : panel.read.state === "unavailable" ? panel.read.problem
                          : panel.newest === null ? qsTr("Nothing to report")
                          : panel.newest.summary
                    color: panel.read.state === "unavailable" ? Theme.fillCritical : fold.palette.windowText
                    elide: Label.ElideRight
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                    opacity: panel.newest === null && panel.read.state !== "unavailable" ? 0.6 : 1
                }

                Item {
                    Layout.fillWidth: true
                    visible: panel.open
                }

                Label {
                    visible: !panel.open && panel.newest !== null
                    text: panel.newest ? Estate.ago(panel.newest.at, panel.now) : ""
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.6
                }

                Label {
                    text: Theme.iconsInstalled ? (panel.open ? Theme.icon.chevronDown : Theme.icon.chevronUp)
                                               : (panel.open ? qsTr("Hide") : qsTr("Show"))
                    font.family: Theme.iconsInstalled ? Theme.iconFamily : Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.7
                }
            }
        }

        // Open: this device's own last word, then Core's messages.
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: panel.open ? Math.min(contentHeight, 168) : 0
            Layout.bottomMargin: panel.open ? Theme.spacing : 0
            visible: panel.open
            clip: true
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            header: Column {
                width: list.width

                Label {
                    width: parent.width
                    visible: panel.read.state === "unavailable" || panel.read.state === "stale"
                    text: panel.read.state === "stale"
                          ? qsTr("Showing %1 — could not refresh: %2")
                            .arg(Qt.formatTime(panel.read.readAt, "HH:mm")).arg(panel.read.problem)
                          : panel.read.problem
                    color: Theme.fillCritical
                    wrapMode: Text.WordWrap
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    bottomPadding: Theme.spacing
                }

                RowLayout {
                    width: parent.width
                    visible: Omnuv.status !== ""
                    spacing: Theme.spacingLoose
                    height: visible ? 28 : 0

                    Pill {
                        text: qsTr("this device")
                    }
                    Label {
                        Layout.fillWidth: true
                        text: Omnuv.status
                        elide: Label.ElideRight
                        font.family: Theme.textFamily
                        font.pixelSize: Theme.bodySize
                    }
                }

                Label {
                    width: parent.width
                    visible: panel.read.state === "current" && panel.rows.length === 0 && Omnuv.status === ""
                    text: qsTr("Nothing to report")
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                    opacity: 0.6
                    height: visible ? 28 : 0
                }
            }

            model: panel.rows

            delegate: RowLayout {
                width: list.width
                height: 28
                spacing: Theme.spacingLoose
                opacity: panel.read.state === "stale" ? 0.6 : 1

                Pill {
                    Layout.preferredWidth: 76
                    tone: panel.tone(modelData.severity)
                    dot: true
                    text: panel.word(modelData.severity)
                }
                Label {
                    Layout.fillWidth: true
                    text: modelData.summary
                    elide: Label.ElideRight
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                }
                Label {
                    text: Estate.ago(modelData.at, panel.now)
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.6
                }
            }
        }
    }
}
