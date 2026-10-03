// Omnuv: the six things a gaming buyer decides about a stream.
//
// **Why six, and why these six.** Upstream's settings page is nine hundred
// lines and covers everything Moonlight can do, which is the right page for
// somebody who came to Moonlight. A person who rented a gaming rig came for the
// rig, and the difference between a good session and a bad one is nearly always
// one of the six below. Everything else stays where it is, one button away, in
// upstream's own style, because it is upstream's.
//
//   1  Optimise for        the only control somebody who does not want to
//                          reason about the other five ever has to touch. It
//                          writes them and then gets out of the way — there is
//                          no stored "preset", deliberately: a radio button
//                          that keeps saying *Balanced* after the bitrate has
//                          been nudged is a lie, and `StreamingPreferences` is
//                          upstream's file and gains no member of ours.
//   2  Resolution and      one decision, not two. They are always chosen
//      frame rate          together, the sensible bitrate is a function of both
//                          (`getDefaultBitrate(w, h, fps, yuv444)`), and both
//                          lists come from this device's own displays and
//                          decoder rather than from a table we invented.
//   3  Bitrate             the one that has to be reachable. Upstream's default
//                          is computed for a LAN; this stream crosses a
//                          WireGuard tunnel over the open internet, and every
//                          "it looks blocky" conversation ends at this slider.
//   4  Display mode        changed on the first stream, by everybody, and twice
//                          more by anybody with a second monitor.
//   5  Frame pacing        the smoothness-against-latency lever, and the one
//                          genuinely worth a buyer's judgement: on a tunnel
//                          with jitter it removes micro-stutter, and it does it
//                          by holding early frames back.
//   6  Audio               stereo or surround. Always works, unlike most of
//                          what it beat, and a headset is what a gaming buyer
//                          is wearing.
//
// What lost, and why, so the next person does not re-argue it:
//
//   HDR                    silently does nothing without host *and* display
//                          support, and a toggle that does nothing is worse
//                          than no toggle. Upstream's page says so beside it;
//                          a six-item sheet has no room to.
//   Video codec, decoder   fixes, not choices. Somebody reaching for AV1 is
//                          debugging, and debugging belongs on the page that
//                          explains itself.
//   Mouse mode, system     right by default for a game, and wrong to change
//   keys, gamepad          without understanding. Nobody arrives wanting them.
//   V-Sync                 on by default and left there. It is frame pacing's
//                          precondition upstream (`SettingsView.qml:840`), so
//                          the switch below is disabled rather than lying when
//                          somebody has turned V-Sync off in Advanced.
//   Performance overlay    replaced rather than exposed: I4 puts the same
//                          numbers on a quality glyph in the stream, and
//                          shipping the text overlay now would be two answers
//                          to one question.
//
// Every value here is upstream's `StreamingPreferences` singleton
// (`app/main.cpp:970`), written straight through and saved when the sheet
// closes. Nothing is stored in Omnuv's own settings: a second copy of a
// preference is a second answer, and the stream reads upstream's.

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import StreamingPreferences 1.0
import SystemProperties 1.0

import Omnuv 1.0

Dialog {
    id: sheet

    // The view above pushes upstream's page; this sheet does not know what a
    // stack is.
    signal advancedRequested()

    // **Advanced writes the same preferences, and this follows them** (2
    // October 2026: 165 FPS set in Advanced, and this sheet still said 60).
    // Each control read the preferences once, when it was made, and the
    // sheet outlives a visit to Advanced. Deferred with callLater because the
    // sheet's own resolution writes width and then height: a sync between the
    // two would see a size nobody chose and append it.
    Connections {
        target: StreamingPreferences
        function onDisplayModeChanged() {
            Qt.callLater(resolution.syncFromPreferences)
            Qt.callLater(frameRate.syncFromPreferences)
        }
        function onBitrateChanged() { Qt.callLater(sheet.syncBitrate) }
        function onWindowModeChanged() { Qt.callLater(displayMode.syncFromPreferences) }
        function onAudioConfigChanged() { Qt.callLater(audio.syncFromPreferences) }
        function onFramePacingChanged() { Qt.callLater(sheet.syncSmoothMotion) }
        function onEnableVsyncChanged() { Qt.callLater(sheet.syncSmoothMotion) }
    }
    // This display's refresh rate, read when the frame-rate list is built,
    // and whether a stream rate lands on it evenly.
    property int displayRate: 0
    function evenOn(rate) { return displayRate > 0 && rate > 0 && displayRate % rate === 0 }
    // An uneven rate is held to an even beat rather than dropped; Smooth
    // motion needs V-Sync, so without it nothing is changed.
    function paceUnevenRate() {
        if (displayRate > 0 && !evenOn(StreamingPreferences.fps) && StreamingPreferences.enableVsync)
            StreamingPreferences.framePacing = true
    }
    // A slider or switch the person has moved has lost its binding, so these
    // set the value rather than rebind it.
    function syncBitrate() { bitrate.value = StreamingPreferences.bitrateKbps }
    function syncSmoothMotion() {
        smoothMotion.checked = StreamingPreferences.enableVsync && StreamingPreferences.framePacing
    }

    title: qsTr("Stream settings")
    header: OmnuvDialogTitle { text: sheet.title }
    modal: true
    standardButtons: Dialog.Close
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    width: Math.min(parent.width - 80, 460)

    // A dialog is a top-level container, so 8 and not 4.
    background: Rectangle {
        radius: Theme.radiusOverlay
        color: Theme.fillCard
        border.width: 1
        border.color: Theme.strokeCard
    }

    // Written once, when the sheet is dismissed, rather than on every drag of
    // the bitrate slider: `save()` writes the whole QSettings file.
    onClosed: StreamingPreferences.save()

    // ---- The one derived value everything else feeds ---------------------
    //
    // Upstream recomputes the bitrate from resolution and frame rate whenever
    // either moves and `autoAdjustBitrate` is on, and this does the same thing
    // in one place instead of at three call sites.
    function retuneBitrate() {
        if (!StreamingPreferences.autoAdjustBitrate) {
            return
        }
        StreamingPreferences.bitrateKbps = defaultBitrate()
        bitrate.value = StreamingPreferences.bitrateKbps
    }

    function defaultBitrate() {
        return StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                      StreamingPreferences.height,
                                                      StreamingPreferences.fps,
                                                      StreamingPreferences.enableYUV444)
    }

    /**
     * Apply one preset.
     *
     * The three differ in resolution and in whether frames are held back, and
     * in nothing else — in particular not in frame rate, which stays at
     * whatever this device's display can actually do. A preset that set 120 on
     * a 60 Hz panel would be a number chosen by a table rather than by the
     * hardware, which is the mistake this whole file exists to avoid.
     *
     * The bitrate follows from the resolution, so it is not part of the preset
     * either; it is recomputed from it.
     *
     * **These are a starting point, not a calibration.** Every network reads
     * differently, which is why the five controls underneath stay visible and
     * show exactly what the preset just did: the knob is meant to be turned.
     */
    function applyPreset(width, height, pacing) {
        StreamingPreferences.width = width
        StreamingPreferences.height = height
        StreamingPreferences.autoAdjustBitrate = true
        StreamingPreferences.framePacing = pacing
        retuneBitrate()
        resolution.syncFromPreferences()
        frameRate.syncFromPreferences()
    }

    ColumnLayout {
        width: parent.width
        spacing: Theme.spacingLoose

        // ---- 1 · Optimise for -------------------------------------------
        Label {
            text: qsTr("Optimise for")
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            font.weight: Theme.strongWeight
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Button {
                Layout.fillWidth: true
                text: qsTr("Picture")
                onClicked: sheet.applyPreset(2560, 1440, true)
            }

            Button {
                Layout.fillWidth: true
                text: qsTr("Balance")
                onClicked: sheet.applyPreset(1920, 1080, true)
            }

            Button {
                Layout.fillWidth: true
                text: qsTr("Response")
                onClicked: sheet.applyPreset(1280, 720, false)
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("A starting point. Everything below stays yours to change, and shows what the choice just did.")
            font.family: Theme.textFamily
            font.pixelSize: Theme.captionSize
            opacity: 0.6
            wrapMode: Text.WordWrap
        }

        // ---- 2 · Resolution and frame rate -------------------------------
        Label {
            Layout.topMargin: Theme.spacing
            text: qsTr("Resolution and frame rate")
            font.family: Theme.textFamily
            font.pixelSize: Theme.bodySize
            font.weight: Theme.strongWeight
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            ComboBox {

                popup.background: OmnuvSurface {}
                id: resolution
                Accessible.role: Accessible.ComboBox
                Accessible.name: qsTr("Resolution")
                Layout.fillWidth: true
                textRole: "text"

                // Selects the entry matching what is saved, or appends it. A
                // width and height that came from upstream's own custom-
                // resolution dialog has to stay visible here rather than be
                // silently replaced by the nearest of ours.
                function syncFromPreferences() {
                    for (var i = 0; i < model.count; i++) {
                        if (parseInt(model.get(i).w) === StreamingPreferences.width &&
                            parseInt(model.get(i).h) === StreamingPreferences.height) {
                            currentIndex = i
                            return
                        }
                    }
                    model.append({
                                     "text": qsTr("%1 × %2").arg(StreamingPreferences.width)
                                                            .arg(StreamingPreferences.height),
                                     "w": "" + StreamingPreferences.width,
                                     "h": "" + StreamingPreferences.height
                                 })
                    currentIndex = model.count - 1
                }

                model: ListModel {
                    ListElement { text: "1280 × 720";  w: "1280"; h: "720" }
                    ListElement { text: "1920 × 1080"; w: "1920"; h: "1080" }
                    ListElement { text: "2560 × 1440"; w: "2560"; h: "1440" }
                    ListElement { text: "3840 × 2160"; w: "3840"; h: "2160" }
                }

                Component.onCompleted: {
                    // Upstream's own rule, and its own guard: prune anything
                    // the decoder cannot do, by pixel count, and only once the
                    // maximum is actually known. Zero means not yet loaded, and
                    // pruning against zero would empty the list.
                    // (`SettingsView.qml:186-198`.)
                    var maxPixels = SystemProperties.maximumResolution.width *
                                    SystemProperties.maximumResolution.height
                    if (maxPixels > 0) {
                        for (var j = 0; j < model.count; j++) {
                            if (parseInt(model.get(j).w) * parseInt(model.get(j).h) > maxPixels) {
                                model.remove(j)
                                j--
                            }
                        }
                    }
                    syncFromPreferences()
                }

                onActivated: {
                    StreamingPreferences.width = parseInt(model.get(currentIndex).w)
                    StreamingPreferences.height = parseInt(model.get(currentIndex).h)
                    sheet.retuneBitrate()
                }
            }

            ComboBox {

                popup.background: OmnuvSurface {}
                id: frameRate
                Accessible.role: Accessible.ComboBox
                Accessible.name: qsTr("Frame rate")
                textRole: "text"

                // **Rates that land evenly on this display first** (the
                // operator, 2 October 2026: 165 fps on a 480 Hz monitor
                // rendered 82 and dropped 37% for pacing, while 480 was
                // smooth). With V-Sync a frame waits for a refresh, and a rate
                // that does not divide the refresh misses its slots unevenly.
                // So the list leads with the divisors of this display's rate,
                // and an uneven one says so and turns Smooth motion on.
                function label(rate) {
                    return sheet.evenOn(rate) || sheet.displayRate <= 0
                        ? qsTr("%1 FPS").arg(rate)
                        : qsTr("%1 FPS \u00B7 uneven on this display").arg(rate)
                }
                function syncFromPreferences() {
                    for (var i = 0; i < model.count; i++) {
                        if (parseInt(model.get(i).fps) === StreamingPreferences.fps) {
                            currentIndex = i
                            return
                        }
                    }
                    model.append({ "text": label(StreamingPreferences.fps),
                                   "fps": "" + StreamingPreferences.fps })
                    currentIndex = model.count - 1
                }

                model: ListModel {}

                Component.onCompleted: {
                    // This device's displays decide what is on offer above 60,
                    // exactly as upstream's list does — `SystemProperties
                    // .getRefreshRate(i)` per display until it answers 0, which
                    // is how upstream detects the end of the list
                    // (`SettingsView.qml:594-604`). A rate nothing here can
                    // present is a rate nobody should be able to pick.
                    SystemProperties.refreshDisplays()
                    sheet.displayRate = SystemProperties.getRefreshRate(0)
                    var rates = [30, 60]
                    for (var d = 0; ; d++) {
                        var rate = SystemProperties.getRefreshRate(d)
                        if (rate === 0) {
                            break
                        }
                        rates.push(rate)
                    }
                    // Every divisor of this display's rate from 60 up: on a
                    // 480 Hz screen, 60 80 96 120 160 240 480.
                    for (var k = 1; sheet.displayRate > 0 && sheet.displayRate / k >= 60; k++) {
                        if (sheet.displayRate % k === 0) {
                            rates.push(sheet.displayRate / k)
                        }
                    }
                    var seen = {}
                    rates = rates.filter(function(r) { return seen[r] ? false : (seen[r] = true) })
                    rates.sort(function(a, b) {
                        var ea = sheet.evenOn(a) ? 0 : 1, eb = sheet.evenOn(b) ? 0 : 1
                        return ea !== eb ? ea - eb : a - b
                    })
                    for (var i = 0; i < rates.length; i++) {
                        model.append({ "text": label(rates[i]), "fps": "" + rates[i] })
                    }
                    syncFromPreferences()
                }

                onActivated: {
                    StreamingPreferences.fps = parseInt(model.get(currentIndex).fps)
                    sheet.paceUnevenRate()
                    sheet.retuneBitrate()
                }
            }
        }
        Label {
            Layout.fillWidth: true
            // Only with V-Sync on, the one case where an uneven rate drops
            // frames: with it off each frame is shown as it is decoded, which
            // tears a little and drops nothing, and was the operator's own
            // fix on a 480 Hz monitor.
            visible: sheet.displayRate > 0 && StreamingPreferences.enableVsync
                     && !sheet.evenOn(StreamingPreferences.fps)
            text: qsTr("%1 fps does not divide this display's %2 Hz, so Smooth motion is on: frames are held to an even beat instead of dropped. Turning V-Sync off in Advanced also stops the drops, with a little tearing.")
                      .arg(StreamingPreferences.fps).arg(sheet.displayRate)
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.captionSize
            opacity: 0.75
        }

        // ---- 3 · Bitrate --------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacing
            spacing: Theme.spacing

            Label {
                Layout.fillWidth: true
                text: qsTr("Bitrate")
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                font.weight: Theme.strongWeight
            }

            Label {
                text: qsTr("%1 Mbps").arg(Math.round(StreamingPreferences.bitrateKbps / 100) / 10)
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
            }
        }

        Slider {
            id: bitrate
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Bitrate")
            // Announced in the unit the label shows, not in kbps: a
            // screen reader otherwise reads out five digits nobody uses.
            Accessible.description: qsTr("%1 Mbps").arg(Math.round(StreamingPreferences.bitrateKbps / 100) / 10)
            Layout.fillWidth: true
            from: 500
            // Upstream's own ceiling, and its own escape hatch: the unlocked
            // limit is only reachable from the Advanced page, which is where a
            // number that large should have to be asked for.
            to: StreamingPreferences.unlockBitrate ? 500000 : 150000
            stepSize: 500
            snapMode: Slider.SnapAlways
            value: StreamingPreferences.bitrateKbps
            onMoved: {
                StreamingPreferences.bitrateKbps = value
                // Moved by hand, so stop recomputing it from the resolution —
                // otherwise the next change of either would quietly undo this.
                StreamingPreferences.autoAdjustBitrate = false
            }
        }

        Button {
            Layout.alignment: Qt.AlignRight
            flat: true
            visible: StreamingPreferences.bitrateKbps !== sheet.defaultBitrate()
            text: qsTr("Back to %1 Mbps").arg(Math.round(sheet.defaultBitrate() / 100) / 10)
            onClicked: {
                StreamingPreferences.autoAdjustBitrate = true
                sheet.retuneBitrate()
            }
        }

        // ---- 4 · Display mode ---------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacing
            spacing: Theme.spacing

            Label {
                Layout.fillWidth: true
                text: qsTr("Display")
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                font.weight: Theme.strongWeight
            }

            ComboBox {

                popup.background: OmnuvSurface {}
                id: displayMode
                Accessible.role: Accessible.ComboBox
                Accessible.name: qsTr("Display")
                textRole: "text"
                // Some renderers can only ever be fullscreen, and upstream
                // disables the control rather than offering a choice that will
                // not be honoured (`SettingsView.qml:803`).
                enabled: !SystemProperties.rendererAlwaysFullScreen

                // The enum by name, never by its number: a `ListElement`
                // resolves an enum on the singleton, which is how upstream's
                // own audio list is written (`SettingsView.qml:917-928`), so
                // nothing here breaks if the enum is ever reordered.
                model: ListModel {
                    ListElement { text: qsTr("Fullscreen")
                                  mode: StreamingPreferences.WM_FULLSCREEN }
                    ListElement { text: qsTr("Borderless windowed")
                                  mode: StreamingPreferences.WM_FULLSCREEN_DESKTOP }
                    ListElement { text: qsTr("Windowed")
                                  mode: StreamingPreferences.WM_WINDOWED }
                }

                function syncFromPreferences() {
                    for (var i = 0; i < model.count; i++) {
                        if (model.get(i).mode === StreamingPreferences.windowMode) {
                            currentIndex = i
                            break
                        }
                    }
                }
                Component.onCompleted: syncFromPreferences()

                onActivated: StreamingPreferences.windowMode = model.get(currentIndex).mode
            }
        }

        // ---- 5 · Frame pacing ----------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacing
            spacing: Theme.spacing

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    text: qsTr("Smooth motion")
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.bodySize
                    font.weight: Theme.strongWeight
                }

                Label {
                    Layout.fillWidth: true
                    text: StreamingPreferences.enableVsync
                          ? qsTr("Holds frames that arrive early, so motion is even. Costs a little response.")
                          : qsTr("Needs V-Sync, which is off. It is in Advanced.")
                    font.family: Theme.textFamily
                    font.pixelSize: Theme.captionSize
                    opacity: 0.6
                    wrapMode: Text.WordWrap
                }
            }

            Switch {
                Accessible.role: Accessible.CheckBox
                id: smoothMotion
                Accessible.name: qsTr("Smooth motion")
                // Upstream's gate, kept: frame pacing does nothing without
                // V-Sync (`SettingsView.qml:840-841`), so the switch is
                // disabled rather than allowed to claim something it cannot do.
                enabled: StreamingPreferences.enableVsync
                checked: StreamingPreferences.enableVsync && StreamingPreferences.framePacing
                onToggled: StreamingPreferences.framePacing = checked
            }
        }

        // ---- 6 · Audio ------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacing
            spacing: Theme.spacing

            Label {
                Layout.fillWidth: true
                text: qsTr("Audio")
                font.family: Theme.textFamily
                font.pixelSize: Theme.bodySize
                font.weight: Theme.strongWeight
            }

            ComboBox {

                popup.background: OmnuvSurface {}
                id: audio
                Accessible.role: Accessible.ComboBox
                Accessible.name: qsTr("Audio")
                textRole: "text"

                // AudioConfig is reachable from QML as an enum on the
                // singleton — upstream's own list does exactly this
                // (`SettingsView.qml:917-928`) — so unlike WindowMode above
                // there is no literal to assert.
                model: ListModel {
                    ListElement { text: qsTr("Stereo");  config: StreamingPreferences.AC_STEREO }
                    ListElement { text: qsTr("5.1");     config: StreamingPreferences.AC_51_SURROUND }
                    ListElement { text: qsTr("7.1");     config: StreamingPreferences.AC_71_SURROUND }
                }

                function syncFromPreferences() {
                    for (var i = 0; i < model.count; i++) {
                        if (model.get(i).config === StreamingPreferences.audioConfig) {
                            currentIndex = i
                            break
                        }
                    }
                }
                Component.onCompleted: syncFromPreferences()

                onActivated: StreamingPreferences.audioConfig = model.get(currentIndex).config
            }
        }

        // ---- The other nine hundred lines, one button away -----------------
        Button {
            Layout.topMargin: Theme.spacingLoose
            Layout.alignment: Qt.AlignLeft
            flat: true
            text: qsTr("Advanced…")
            onClicked: {
                sheet.close()
                sheet.advancedRequested()
            }
        }
    }
}
