// Omnuv: the deploy flow, the web's DeployDialog 1:1 (the Instances redesign,
// 3 October 2026; docs/plans/recipes-in-instances.md section 3, in the omnuv
// repository).
//
// Two steps and one write. Step 1 is Core's catalogue
// (`GET /v1/catalog/deployables`): apps, then plain instances, in Core's
// order; a tile that cannot be had now is dimmed with its reason, focusable,
// never hidden. Step 2 configures it, and Deploy sends one `POST /v1/deploy`
// with an idempotency key minted when step 2 opened, so a retry after a
// dropped answer never rents a second machine. A refusal is shown in Core's
// words beside the field it names, with the form kept.
//
// Closing sends nothing; the chosen item and the typed name survive it.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omnuv 1.0

Dialog {
    id: flow
    objectName: "deployFlow"
    modal: true
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 32 : 720, 720)
    height: Math.min(parent ? parent.height - 32 : 760, 760)
    padding: 0
    title: qsTr("Deploy")
    focus: true
    // **Opaque, on a dimmed page** (3 October 2026: the style's base was
    // translucent and the page's cards read through the tiles). The web's
    // surface, and the modal dim the web's dialog sits on.
    background: Rectangle {
        color: Theme.highContrast ? flow.palette.base : Theme.fillCard
        radius: 16
        border.color: Theme.strokeCard
    }
    Overlay.modal: Rectangle { color: "#8C000000" }
    // A popup does not inherit the page's palette: the accent again here, so
    // Deploy and the radios are the web's colour, not the system's.
    Binding { target: flow.palette; property: "accent"; value: Theme.accent; when: !Theme.highContrast }
    Binding { target: flow.palette; property: "highlight"; value: Theme.accent; when: !Theme.highContrast }

    property int step: 1
    property var chosen: null
    property string key: ""
    property string sizeId: ""
    property string gpuModel: ""
    property int customVcpus: 4
    property int customMemory: 8
    property int customDisk: 80
    property bool sending: false
    property string refusedField: ""
    property string refusedText: ""

    readonly property var items: Omnuv.deployables
    readonly property var apps: items.filter(function (d) { return d.group === "apps" })
    readonly property var plain: items.filter(function (d) { return d.group !== "apps" })
    readonly property string nameProblem: Omnuv.nameProblem(nameField.text, countBox.value)
    function estimate() {
        if (chosen === null || sizeId === "custom") return ""
        var size = (chosen.sizes || []).filter(function (z) { return z.id === sizeId })[0]
        if (!size || size.price_per_hour === undefined) return ""
        var card = "0"
        if (gpuModel !== "") {
            var own = (chosen.gpus || []).filter(function (g) { return g.model === gpuModel })[0]
            var sale = Omnuv.saleGpus.filter(function (g) { return g.model === gpuModel })[0]
            card = own ? own.price_per_hour : (sale ? sale.price : undefined)
            if (card === undefined) return ""
        }
        var units = function (x) { return Math.round(Number(x) * 10000) }
        var total = (units(size.price_per_hour) + units(card)) * countBox.value
        return "€" + (total / 10000).toFixed(4) + "/h"
    }
    readonly property bool gpuMissing: chosen !== null && chosen.gpu === "required" && gpuModel === ""
    readonly property string blockedBecause: nameField.text.trim() === "" ? qsTr("Give it a name.")
        : nameProblem !== "" ? nameProblem
        : gpuMissing ? qsTr("No GPU this needs is free right now.")
        : ""

    function at(field) { return refusedField === field ? refusedText : "" }

    function defaultName(d) {
        var stems = { chat: "chat", web: "web", game: "rig", windows: "win", gpu: "gpu", linux: "ubuntu" }
        return (stems[d.mark] || "instance") + "-1"
    }
    // Selects a tile: a click or Space. Step 1 stays open.
    function pick(d) {
        if (!d.available) return
        if (chosen === null || chosen.id !== d.id) fill(d)
    }
    function fill(d) {
        chosen = d
        nameField.text = defaultName(d)
        sizeId = d.sizes.length > 0 ? d.sizes[0].id : "custom"
        countBox.value = 1
        // An app that may use a card gets the cheapest free one; a plain
        // instance starts without, and the buyer adds one.
        var free = Omnuv.saleGpus.filter(function (g) { return g.free > 0 })
        gpuModel = (d.gpu === "required" || (d.gpu === "optional" && d.kind === "recipe")) && free.length > 0 ? free[0].model : ""
    }
    // Goes to step 2: Enter, a double click, Next, or a deep link.
    function choose(d) {
        if (!d.available) return
        if (chosen === null || chosen.id !== d.id) fill(d)
        key = Omnuv.newIdempotencyKey()
        refusedField = ""
        refusedText = ""
        step = 2
        nameField.forceActiveFocus()
    }
    function back() {
        step = 1
        refusedField = ""
        refusedText = ""
        Qt.callLater(focusTile)
    }
    function deploy() {
        if (chosen === null || blockedBecause !== "" || sending) return
        var body = {
            item: { kind: chosen.kind, id: chosen.id },
            name: nameField.text.trim(),
            count: countBox.value
        }
        if (sizeId === "custom") body.custom = { vcpus: customVcpus, memory_gib: customMemory, disk_gib: customDisk }
        else body.size = sizeId
        if (gpuModel !== "") body.gpu = { model: gpuModel, count: 1 }
        if (chosen.olderCore) body.olderCore = true
        sending = true
        refusedField = ""
        refusedText = ""
        Omnuv.deployItem(body, key)
    }

    onAboutToShow: {
        if (chosen === null) step = 1
        Omnuv.loadDeployables()
    }
    // Focus starts on the chosen tile, else the first that can be had.
    onOpened: Qt.callLater(focusTile)
    onItemsChanged: if (opened && step === 1) Qt.callLater(focusTile)

    // The tiles, by id, as they register; arrows move among the available
    // ones in the catalogue's order (apps, then plain instances).
    property var tiles: ({})
    readonly property var order: apps.concat(plain).filter(function (d) { return d.available }).map(function (d) { return d.id })
    function focusTile(id) {
        if (step !== 1) return
        var at = id || (chosen !== null && order.indexOf(chosen.id) >= 0 ? chosen.id : order[0])
        if (at && tiles[at]) tiles[at].forceActiveFocus(Qt.TabFocusReason)
    }
    function moveTile(from, step) {
        var i = order.indexOf(from)
        if (order.length === 0) return
        focusTile(order[(i < 0 ? 0 : (i + step + order.length) % order.length)])
    }

    Connections {
        target: Omnuv
        function onDeployRefused(field, code, message) {
            flow.sending = false
            flow.refusedField = field
            flow.refusedText = message
        }
        function onDeployed(instances) {
            flow.sending = false
            // Done with this configuration: the next deploy is a new one.
            flow.chosen = null
            flow.step = 1
            flow.close()
        }
    }

    header: ColumnLayout {
        spacing: 2
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 12
            Layout.topMargin: 12
            Label {
                Layout.fillWidth: true
                Layout.topMargin: 8
                text: qsTr("Deploy")
                font.family: Theme.displayFamily
                font.pixelSize: 18
                font.weight: Theme.strongWeight
            }
            // The design's [×], as the web's: Esc was the only way out.
            ToolButton {
                objectName: "deployClose"
                text: "\u2715"
                enabled: !flow.sending
                Accessible.name: qsTr("Close")
                onClicked: flow.close()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.bottomMargin: 12
            Label {
                Layout.fillWidth: true
                objectName: "deployStep"
                text: flow.step === 1 ? qsTr("Step 1 of 2 · Choose what to deploy")
                                      : qsTr("Step 2 of 2 · %1").arg(flow.chosen ? flow.chosen.name : "")
                font.pixelSize: 13
                opacity: 0.7
                elide: Label.ElideRight
            }
            Button {
                visible: flow.step === 2
                flat: true
                text: qsTr("Change")
                onClicked: flow.back()
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.strokeCard }
    }

    contentItem: ScrollView {
        id: scroller
        clip: true
        contentWidth: availableWidth

        // One child, as a ScrollView measures one: the two steps inside it.
        ColumnLayout {
        width: scroller.availableWidth
        spacing: 0

        // ---- Step 1: the catalogue ---------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: flow.step === 1
            spacing: 16

            Item { implicitHeight: 4 }

            Label {
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.fillWidth: true
                visible: Omnuv.deployablesProblem !== ""
                text: Omnuv.deployablesProblem
                color: Theme.fillCaution
                wrapMode: Text.WordWrap
            }
            Button {
                Layout.leftMargin: 24
                visible: Omnuv.deployablesProblem !== ""
                text: qsTr("Try again")
                onClicked: Omnuv.loadDeployables()
            }
            Label {
                Layout.leftMargin: 24
                visible: flow.items.length === 0 && Omnuv.deployablesProblem === ""
                text: qsTr("Reading what can be deployed…")
                opacity: 0.7
            }

            Repeater {
                model: [ { title: qsTr("Apps"), list: flow.apps }, { title: qsTr("Plain instances"), list: flow.plain } ]

                ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Layout.rightMargin: 24
                    visible: modelData.list.length > 0
                    spacing: 8

                    Label {
                        text: modelData.title.toUpperCase()
                        font.pixelSize: 12
                        font.letterSpacing: 0.6
                        opacity: 0.6
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: scroller.availableWidth >= 560 ? 3 : 1
                        columnSpacing: 12
                        rowSpacing: 12

                        Repeater {
                            model: modelData.list

                            ItemDelegate {
                                id: tile
                                required property var modelData
                                objectName: "tile-" + modelData.id
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.alignment: Qt.AlignTop
                                Layout.preferredWidth: 1
                                implicitHeight: tileColumn.implicitHeight + 24
                                checkable: false
                                focusPolicy: Qt.StrongFocus
                                readonly property bool selected: flow.chosen !== null && flow.chosen.id === modelData.id
                                Accessible.role: Accessible.RadioButton
                                Accessible.checkable: true
                                Accessible.checked: selected
                                Accessible.name: modelData.name
                                Accessible.description: modelData.available ? modelData.summary : modelData.unavailable_because
                                opacity: modelData.available ? 1 : 0.55
                                onClicked: flow.pick(modelData)
                                onDoubleClicked: flow.choose(modelData)
                                Component.onCompleted: flow.tiles[modelData.id] = tile
                                Keys.onSpacePressed: flow.pick(modelData)
                                Keys.onReturnPressed: flow.choose(modelData)
                                Keys.onEnterPressed: flow.choose(modelData)
                                Keys.onRightPressed: flow.moveTile(modelData.id, 1)
                                Keys.onDownPressed: flow.moveTile(modelData.id, 1)
                                Keys.onLeftPressed: flow.moveTile(modelData.id, -1)
                                Keys.onUpPressed: flow.moveTile(modelData.id, -1)

                                background: Rectangle {
                                    radius: 12
                                    color: tile.selected ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.10)
                                         : tile.hovered && tile.modelData.available ? Theme.fillSubtle : "transparent"
                                    border.width: tile.selected || tile.visualFocus ? 2 : 1
                                    border.color: tile.selected || tile.visualFocus ? Theme.accent : Theme.strokeCard
                                }
                                contentItem: ColumnLayout {
                                    id: tileColumn
                                    spacing: 6
                                    WorkloadMark {
                                        Layout.preferredWidth: 40
                                        Layout.preferredHeight: 40
                                        workload: tile.modelData.mark
                                        // The eye where a card is the point: an image that
                                        // needs one, or an app that can use one (section 3).
                                        hasGpu: tile.modelData.gpu === "required" || (tile.modelData.gpu === "optional" && tile.modelData.kind === "recipe")
                                        asleep: !tile.modelData.available
                                        Accessible.ignored: true
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: tile.modelData.name
                                        font.weight: Theme.strongWeight
                                        wrapMode: Text.WordWrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: tile.modelData.summary
                                        font.pixelSize: 13
                                        opacity: 0.7
                                        wrapMode: Text.WordWrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: [tile.modelData.gpu === "required" ? qsTr("GPU") : tile.modelData.gpu === "optional" ? qsTr("GPU optional") : "",
                                               tile.modelData.typical_secs ? qsTr("~%1 min to install").arg(Math.max(1, Math.round(tile.modelData.typical_secs / 60))) : "",
                                               tile.modelData.mark === "game" ? qsTr("Plays with the Omnuv app") : "",
                                               tile.modelData.price_from_per_hour ? qsTr("from €%1/h").arg(Number(tile.modelData.price_from_per_hour).toFixed(2)) : ""]
                                              .filter(function (p) { return p !== "" }).join(" · ")
                                        visible: text !== ""
                                        font.pixelSize: 12
                                        opacity: 0.6
                                        wrapMode: Text.WordWrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: !tile.modelData.available
                                        text: tile.modelData.unavailable_because || ""
                                        color: Theme.fillCaution
                                        font.pixelSize: 12
                                        font.weight: Theme.strongWeight
                                        wrapMode: Text.WordWrap
                                    }
                                    Item { Layout.fillHeight: true }
                                }
                            }
                        }
                    }
                }
            }
            Item { implicitHeight: 8 }
        }

        // ---- Step 2: configure -------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: flow.step === 2 && flow.chosen !== null
            spacing: 18

            Item { implicitHeight: 4 }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                spacing: 12
                WorkloadMark {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    workload: flow.chosen ? flow.chosen.mark : "linux"
                    hasGpu: flow.gpuModel !== ""
                    Accessible.ignored: true
                }
                Label {
                    Layout.fillWidth: true
                    text: flow.chosen ? flow.chosen.name : ""
                    font.weight: Theme.strongWeight
                }
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                visible: flow.at("item") !== ""
                text: flow.at("item")
                color: Theme.fillCaution
                wrapMode: Text.WordWrap
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                spacing: 4
                Label { text: qsTr("Name"); font.weight: Theme.strongWeight }
                TextField {
                    id: nameField
                    objectName: "deployName"
                    Layout.fillWidth: true
                    font.family: Theme.monoFamily
                    Accessible.name: qsTr("Name")
                    Accessible.description: nameHint.text
                    onAccepted: flow.deploy()
                }
                Label {
                    id: nameHint
                    objectName: "deployNameHint"
                    Layout.fillWidth: true
                    text: flow.at("name") !== "" ? flow.at("name")
                          : nameField.text !== "" && flow.nameProblem !== "" ? flow.nameProblem
                          : qsTr("Letters, digits and hyphens, up to 48. It becomes its name on your network.")
                    color: flow.at("name") !== "" || (nameField.text !== "" && flow.nameProblem !== "") ? Theme.fillCaution : palette.windowText
                    opacity: color === palette.windowText ? 0.6 : 1
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                spacing: 4
                Label { text: qsTr("Size"); font.weight: Theme.strongWeight }
                ButtonGroup { id: sizeGroup }
                Repeater {
                    model: flow.chosen ? flow.chosen.sizes : []
                    RadioButton {
                        required property var modelData
                        ButtonGroup.group: sizeGroup
                        checked: flow.sizeId === modelData.id
                        text: (flow.chosen && flow.chosen.kind === "recipe" ? qsTr("Its size") : modelData.name) + "   " + modelData.label
                        onClicked: flow.sizeId = modelData.id
                    }
                }
                RadioButton {
                    visible: flow.chosen !== null && flow.chosen.kind === "image"
                    ButtonGroup.group: sizeGroup
                    checked: flow.sizeId === "custom"
                    text: qsTr("Custom")
                    onClicked: flow.sizeId = "custom"
                }
                RowLayout {
                    visible: flow.sizeId === "custom"
                    Layout.leftMargin: 28
                    spacing: 12
                    Label { text: qsTr("vCPU") }
                    SpinBox { from: 1; to: 32; value: flow.customVcpus; editable: true; onValueModified: flow.customVcpus = value; Accessible.name: qsTr("vCPU") }
                    Label { text: qsTr("Memory, GiB") }
                    SpinBox { from: 1; to: 64; value: flow.customMemory; editable: true; onValueModified: flow.customMemory = value; Accessible.name: qsTr("Memory, GiB") }
                    Label { text: qsTr("Disk, GiB") }
                    SpinBox { from: 20; to: 500; value: flow.customDisk; editable: true; onValueModified: flow.customDisk = value; Accessible.name: qsTr("Disk, GiB") }
                }
                Label {
                    visible: flow.at("size") !== "" || flow.at("custom") !== ""
                    text: flow.at("size") !== "" ? flow.at("size") : flow.at("custom")
                    color: Theme.fillCaution
                    font.pixelSize: 12
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                visible: flow.chosen !== null && flow.chosen.gpu !== "none"
                spacing: 4
                Label { text: qsTr("GPU"); font.weight: Theme.strongWeight }
                ButtonGroup { id: gpuGroup }
                RadioButton {
                    visible: flow.chosen !== null && flow.chosen.gpu === "optional"
                    ButtonGroup.group: gpuGroup
                    checked: flow.gpuModel === ""
                    text: qsTr("None")
                    onClicked: flow.gpuModel = ""
                }
                Repeater {
                    model: Omnuv.saleGpus
                    RadioButton {
                        required property var modelData
                        ButtonGroup.group: gpuGroup
                        enabled: modelData.free > 0
                        checked: flow.gpuModel === modelData.model
                        text: qsTr("%1 × 1   %2").arg(modelData.model)
                              .arg(modelData.free > 0 ? qsTr("%1 free").arg(modelData.free) : qsTr("none free"))
                        onClicked: flow.gpuModel = modelData.model
                    }
                }
                Label {
                    visible: Omnuv.saleGpus.length === 0
                    text: qsTr("No GPU is listed right now.")
                    opacity: 0.7
                }
                Label {
                    visible: flow.at("gpu") !== ""
                    text: flow.at("gpu")
                    color: Theme.fillCaution
                    font.pixelSize: 12
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                spacing: 4
                Label { text: qsTr("How many"); font.weight: Theme.strongWeight }
                SpinBox {
                    id: countBox
                    objectName: "deployCount"
                    from: 1
                    to: 8
                    editable: true
                    Accessible.name: qsTr("How many")
                }
                Label {
                    visible: countBox.value > 1 && flow.nameProblem === "" && nameField.text.trim() !== ""
                    text: qsTr("Named %1-1 to %1-%2.").arg(nameField.text.trim()).arg(countBox.value)
                    font.pixelSize: 12
                    opacity: 0.7
                }
                Label {
                    visible: flow.at("count") !== ""
                    text: flow.at("count")
                    color: Theme.fillCaution
                    font.pixelSize: 12
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                visible: flow.chosen !== null && flow.chosen.kind === "image"
                text: qsTr("Signs in with your project's SSH keys.")
                opacity: 0.7
                wrapMode: Text.WordWrap
            }

            Rectangle { Layout.fillWidth: true; Layout.leftMargin: 24; Layout.rightMargin: 24; implicitHeight: 1; color: Theme.strokeCard }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                objectName: "deployEstimate"
                // The web's estimate, word for word (DeployDialog.svelte): Core's
                // price for the size plus the card's, times how many, summed in
                // ten-thousandths so 0.1 + 0.2 stays 0.3; none for a custom size,
                // which Core prices only when it makes one.
                text: {
                    var e = flow.estimate()
                    if (e !== "") return qsTr("Estimate %1%2 · nothing is charged in closed testing.")
                                          .arg(e).arg(countBox.value > 1 ? qsTr(" for %1").arg(countBox.value) : "")
                    if (flow.sizeId === "custom") return qsTr("An estimate is shown for the preset sizes · nothing is charged in closed testing.")
                    return qsTr("Nothing is charged in closed testing.")
                }
                opacity: 0.7
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                visible: flow.chosen !== null && (flow.chosen.first_use || "") !== ""
                text: flow.chosen ? (flow.chosen.first_use || "") : ""
                wrapMode: Text.WordWrap
            }
            Label {
                objectName: "deployRefusal"
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                visible: flow.refusedText !== "" && ["name", "size", "custom", "gpu", "count", "item"].indexOf(flow.refusedField) < 0
                text: flow.refusedText
                color: Theme.fillCaution
                wrapMode: Text.WordWrap
            }
            Item { implicitHeight: 8 }
        }
        }
    }

    footer: ColumnLayout {
        spacing: 0
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.strokeCard }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 16
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            spacing: 8
            Label {
                Layout.fillWidth: true
                visible: flow.step === 2
                text: flow.blockedBecause
                font.pixelSize: 12
                opacity: 0.7
                elide: Label.ElideRight
            }
            Item { Layout.fillWidth: true; visible: flow.step === 1 }
            Button {
                text: flow.step === 1 ? qsTr("Cancel") : qsTr("Back")
                enabled: !flow.sending
                onClicked: flow.step === 1 ? flow.close() : flow.back()
            }
            Button {
                objectName: "deployConfirm"
                highlighted: true
                text: flow.step === 1 ? qsTr("Next") : flow.sending ? qsTr("Deploying…") : qsTr("Deploy")
                // "Deploy chat-1", not "Deploy": the page's own Deploy is
                // still in the tree behind the dialog, and a screen reader or
                // the rig's journey must be able to tell the two apart.
                Accessible.name: flow.step === 1 ? qsTr("Next") : qsTr("Deploy %1").arg(nameField.text.trim())
                Accessible.description: flow.step === 2 ? flow.blockedBecause : ""
                enabled: flow.step === 1 ? (flow.chosen !== null && flow.chosen.available)
                                         : flow.blockedBecause === "" && !flow.sending
                onClicked: flow.step === 1 ? flow.choose(flow.chosen) : flow.deploy()
            }
        }
    }
}
