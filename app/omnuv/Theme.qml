pragma Singleton

import QtQuick

import Omnuv 1.0

// How big, how round, how fast, and with which glyph — in one place, so that a
// view says what it means rather than what it measures.
//
// Everything here is Microsoft's number, not ours. Windows publishes a
// geometry, a type ramp, a motion table and an icon font, and an application
// that picks its own looks foreign on the machine it is running on. The values
// below carry the name Microsoft gives them in a comment, so the next person
// can check one against the documentation instead of trusting this file.
//
// **It reads the operating system through `Omnuv.appearance` and nowhere
// else.** That object already reads the two settings we care about and already
// re-reads them when Windows says they moved (`WM_SETTINGCHANGE`); a second
// reader here would be a second answer to the same question, and the two would
// disagree on the day the first one changed. One reader.
//
// For the same reason there is no `Theme.dark`: `Omnuv.appearance.darkAppsTheme`
// is already reachable from every file that imports Omnuv, and a passthrough
// property is a second name for one value. Colour does not come from here at
// all — the style draws the controls and the system palette colours them, which
// is the whole point of D0's "native by style, not by rewrite".
//
// Registered from C++ beside the other Omnuv types, in
// `omnuvsession.cpp:registerOmnuvTypes()`, because this project is qmake and
// has no QML module: `qt_add_qml_module` is a CMake command and there is no
// `qmldir` anywhere in the tree. So `import Omnuv 1.0` — which most files
// already have — brings `Theme` with it.
QtObject {
    // ---- Geometry -------------------------------------------------------
    //
    // "Geometry in Windows 11": 8px on top-level containers — app windows,
    // flyouts, dialogs — and 4px on in-page elements: buttons, fields, list
    // backplates, bars. The two global resources Windows itself exposes are
    // `OverlayCornerRadius` (8) and `ControlCornerRadius` (4), and these are
    // those, under names that say where they go rather than what they are.
    //
    // A window that is snapped or maximised is square, and Windows does that
    // itself for the top-level frame: nothing here has to know.
    readonly property int radiusOverlay: 8
    readonly property int radiusControl: 4
    // A card that carries a picture — Store, Xbox, the Settings home page —
    // is drawn at the overlay radius rather than the control one. A machine
    // card is that kind of card, so it borrows the number and says why.
    readonly property int radiusCard: radiusOverlay

    // ---- Type -----------------------------------------------------------
    //
    // The Windows type ramp, in effective pixels, so `font.pixelSize` and not
    // `font.pointSize` — a point size would be scaled twice on a high-DPI
    // display and stop matching the shell beside it.
    //
    // `Segoe UI Variable` is the Windows 11 family; `Segoe UI` is the Windows
    // 10 one and is the fallback rather than a second design, which is exactly
    // what Microsoft's own title-bar guidance says to do: "Segoe UI Variable
    // (if available) or Segoe UI".
    //
    // Two rules from the same page that this file cannot enforce and every
    // view must follow: sentence case everywhere, and **no bold and no
    // italic** — the ramp has neither, and emphasis is Semibold.
    readonly property var textFamilies: ["Segoe UI Variable", "Segoe UI"]

    // **QML's `font` has `family` and no `families`.** The value type exposes
    // one name (`QQuickFontValueType`: family, styleName, bold, weight, italic,
    // …), and a binding to `font.families` is refused at compile time —
    // `Cannot assign to non-existent property "families"` — which takes the
    // whole file with it, and every file that names that file as a type.
    // That was the blank window of 15 September: twenty-nine such bindings,
    // and the first of them was enough.
    //
    // So the fallback is chosen here, once, from what this machine actually
    // has installed, and consumers bind the one string: `font.family:
    // Theme.textFamily`. The lists above and below stay as the statement of
    // intent; this is how it is applied.
    function firstInstalled(candidates) {
        var installed = Qt.fontFamilies()
        for (var i = 0; i < candidates.length; i++) {
            if (installed.indexOf(candidates[i]) >= 0) {
                return candidates[i]
            }
        }
        return candidates[candidates.length - 1]
    }
    readonly property string textFamily: firstInstalled(textFamilies)

    // Segoe UI Variable has an optical-size axis, and Windows publishes its
    // three stops as families of their own: Small, Text and Display. The type
    // ramp uses Display from Subtitle (20px) up, where its tighter spacing and
    // finer joins are what make a Windows 11 title look like one. Where it is
    // not installed the text family is the answer, which is what it was.
    readonly property string displayFamily: firstInstalled(["Segoe UI Variable Display"].concat(textFamilies))

    // A machine's private name is something a person copies into a terminal,
    // so it is drawn in the face a terminal uses. Cascadia ships with Windows
    // 11; Consolas with everything before it. `monospace` is not a family on
    // Windows and resolved to Courier New, which is nobody's idea of native.
    readonly property string monoFamily: firstInstalled(["Cascadia Mono", "Cascadia Code", "Consolas", "monospace"])

    // Caption 12/16 Regular. Microsoft's stated floor for legibility is 12px
    // Regular, so nothing in this application goes below it.
    readonly property int captionSize: 12
    readonly property int captionLineHeight: 16

    // Body 14/20 Regular, and Body Strong is the same size at `strongWeight` —
    // the weight is the whole difference between them. 14px Semibold is the
    // other floor.
    readonly property int bodySize: 14
    readonly property int bodyLineHeight: 20

    // Subtitle 20/28 Semibold.
    readonly property int subtitleSize: 20
    readonly property int subtitleLineHeight: 28

    // Title 28/36 Semibold.
    readonly property int titleSize: 28
    readonly property int titleLineHeight: 36

    // The ramp has two weights and no more. Semibold, never Bold:
    // `Font.DemiBold` is 600 on the variable font's weight axis, which is what
    // "Body Strong", "Subtitle" and "Title" all mean.
    readonly property int regularWeight: Font.Normal
    readonly property int strongWeight: Font.DemiBold

    // ---- Motion ---------------------------------------------------------
    //
    // **Every duration collapses to zero when the person has asked for reduced
    // motion**, which is what `Omnuv.appearance.animationsEnabled` reports.
    // Zero is not "no transition": the property still changes and still lands
    // on its new value, it simply arrives at once. So a transition that was
    // carrying a meaning still delivers it — the meaning is in the end state,
    // and the movement was only the escort.
    //
    // The one case that is not true of is an animation that *is* the message
    // rather than the escort — the tray's connecting ring, where the fact that
    // something is turning is the entire answer to "is it working". A zero
    // duration would make that read as stopped, which is the opposite of what
    // it means, so it does not collapse: D1 has it fall back to a static
    // connecting glyph instead. That is a different glyph, not a shorter
    // animation, which is why it belongs to the tray and not to a number here.
    //
    // Names and values are Windows': ControlFasterAnimationDuration 83,
    // ControlFastAnimationDuration 167, ControlNormalAnimationDuration 250,
    // and 333 for the slowest of the three timings in the motion table.
    readonly property bool motion: Omnuv.appearance.animationsEnabled

    readonly property int durationFaster: motion ? 83 : 0
    readonly property int durationFast: motion ? 167 : 0
    readonly property int durationNormal: motion ? 250 : 0
    readonly property int durationSlow: motion ? 333 : 0

    // The easing curves, as control points rather than as an `Easing` value,
    // because Windows names three cubic Béziers and Qt has no enum for any of
    // them. A consumer writes:
    //
    //     NumberAnimation {
    //         duration: Theme.durationNormal
    //         easing.type: Easing.Bezier
    //         easing.bezierCurve: Theme.easeEntrance
    //     }
    //
    // The trailing `1, 1` is the curve's end point, which Qt requires in the
    // list. Under reduced motion the duration is zero and the curve is moot,
    // so there is nothing to collapse here.
    //
    // Entrance — "Fast Out, Slow In", for anything arriving.
    readonly property var easeEntrance: [0, 0, 0, 1, 1, 1]
    // Exit — "Slow Out, Fast In", for anything leaving.
    readonly property var easeExit: [1, 0, 1, 1, 1, 1]
    // Point to point, for something already on screen that is moving.
    readonly property var easePointToPoint: [0.55, 0.55, 0, 1, 1, 1]

    // ---- Accent ---------------------------------------------------------
    //
    // The colour the person chose in Personalization, read through Qt's system
    // palette rather than through `UISettings`, which would cost a WinRT
    // dependency for one colour.
    //
    // `accent` was added to Qt's palette in 6.6 and this fork builds against
    // 6.11, but a palette that was never given one falls back to `highlight` —
    // Qt documents that default, and reading a property a Qt build does not
    // have yields `undefined` rather than an error, so the test below covers
    // both cases with one expression.
    //
    // Not conditioned on "show accent colour on title bars": that setting tells
    // the window manager what to paint on the caption, which the window manager
    // then does. It is not exposed to applications and does not change the
    // accent inside the window.
    readonly property SystemPalette systemPalette: SystemPalette {
        colorGroup: SystemPalette.Active
    }

    // **The palette the view's own controls are drawn from**, handed over by
    // `OmnuvView`'s hidden Control, because that is the only thing that knows
    // what the style and the system between them decided. `SystemPalette` is
    // Qt's *other* answer to the same question and the two disagree — the
    // Linux loop's container reports a light system palette under a dark
    // style, which is what put near-white cards under white text on 17
    // September, twice: once for the card fill and once for high contrast.
    // One reader, and `systemPalette` only as the fallback before the view
    // exists.
    property var probe: null
    function fromPalette(name, fallback) {
        return probe !== null && probe[name] !== undefined ? probe[name] : fallback
    }

    readonly property color accent: highContrast ? fromPalette("highlight", systemPalette.highlight)
                                  : systemPalette.accent !== undefined ? systemPalette.accent
                                  : systemPalette.highlight

    // ---- Spacing --------------------------------------------------------
    //
    // Windows lays out on a 4px grid — "Layout" in the Windows 11 design
    // guidance says every gap and every padding is a multiple of 4 — and names
    // the steps by what separates rather than by how big they are. These four
    // are that grid, stopped where this application actually needs it: a
    // fifth step would be a number nobody had a use for.
    //
    // They exist because a margin typed into a view is the same defect as a
    // radius typed into a view: it is nobody's number, it disagrees with the
    // one three files away, and nothing can tell you which of the two is
    // wrong.
    readonly property int spacingTight: 4   // inside one thing: a dot and its word
    readonly property int spacing: 8        // between related things in a row
    readonly property int spacingLoose: 12  // between groups inside a surface
    readonly property int padding: 16       // a surface's own inset

    // ---- Surface and status colour --------------------------------------
    //
    // The header above says colour does not come from here, and that stays
    // true for everything the style can draw: a button, a field, a list
    // backplate are the style's to colour and this file must not second-guess
    // them.
    //
    // Two things the style cannot draw, and so they are here rather than
    // scattered through the views:
    //
    //   a card       WinUI has a card and Qt Quick Controls does not, so the
    //                surface is ours to paint. These are Microsoft's own
    //                CardBackgroundFillColorDefault and CardStrokeColorDefault,
    //                which are deliberately *translucent* — they are designed
    //                to sit on Mica and let it through, which is what D0 put
    //                behind the window.
    //
    //   a status     Windows publishes no "this machine is unhealthy" colour
    //                for a control, because a control does not have one. It
    //                does publish the four system fill colours an InfoBar and
    //                an InfoBadge are tinted with, and a machine's state is
    //                the same kind of fact, so those are what a status dot is
    //                painted with.
    //
    // Verified at the source rather than remembered: fetched
    // microsoft/microsoft-ui-xaml, controls/dev/CommonStyles/
    // Common_themeresources_any.xaml on main, whose ThemeDictionaries are
    // keyed "Default" (dark), "Light" and "HighContrast". SystemFillColor*
    // are at lines 76-79 and 280-283 there, Card* at 46/56 and 250/260.
    // The values below are those, transcribed, in "#AARRGGBB" where Microsoft
    // gave an alpha — which is a form QML's `color` accepts.
    //
    // **High contrast is why the word is never optional.** In that dictionary
    // Microsoft sets all four system fill colours to the same #FF0000, on
    // purpose: under high contrast, colour stops carrying meaning and the text
    // beside it is the only thing left. Every consumer of these draws the
    // state's word too.
    // **Which of those two dictionaries applies is a question about the
    // surface, not about the setting.** The window is painted in the style's
    // own `palette.window` (`gui/main.qml`), and where the style and the
    // system setting disagree — the Linux loop's container is one: no colour
    // scheme, a dark style — choosing by the setting put a 70 % white card
    // under white text. So the view reports the colour its controls are
    // actually drawn on, and the choice follows that. Until it has, the
    // setting decides, as it did before.
    property color surface: "transparent"
    readonly property bool onDarkSurface: surface.a > 0 ? surface.hslLightness < 0.5
                                                        : Omnuv.appearance.darkAppsTheme

    // **High contrast takes every one of these.** Windows replaces its whole
    // palette with a handful of guaranteed-contrasting colours and expects an
    // application to use nothing else; a translucent wash over those is a
    // surface whose contrast nobody can promise. So under it every fill is a
    // palette colour and every edge is drawn — a card is told from the window
    // by its border, not by a shade — and what is only decoration (the cards'
    // pictures, the window's glow, the shadows) is not drawn at all.
    readonly property bool highContrast: Omnuv.appearance.highContrast

    readonly property color fillCard: highContrast ? fromPalette("base", systemPalette.base)
                                    : onDarkSurface ? "#0DFFFFFF" : "#B3FFFFFF"
    readonly property color strokeCard: highContrast ? fromPalette("windowText", systemPalette.windowText)
                                      : onDarkSurface ? "#19000000" : "#0F000000"
    // Pointer over a card: ControlFillColorSecondary, the fill WinUI gives a
    // SettingsCard under the pointer, with ControlStrokeColorSecondary. Under
    // high contrast that is the palette's own selected pair.
    // Under high contrast a card keeps its fill and gains a *bright edge*
    // under the pointer, which is how Windows itself shows hover there. A
    // highlight *fill* would need every label on the card to switch to
    // `highlightedText` in the same instant, and a card whose text did not
    // follow is exactly the unreadable pair this is meant to avoid.
    readonly property color fillCardHover: highContrast ? fillCard
                                         : onDarkSurface ? "#15FFFFFF" : "#80F9F9F9"
    readonly property color strokeCardHover: highContrast ? fromPalette("highlight", systemPalette.highlight)
                                           : onDarkSurface ? "#18FFFFFF" : "#29000000"
    // And pressed: ControlFillColorTertiary, which is *quieter* than either.
    // A press that looked like rest is what this file had until somebody read
    // it — pressing a card took its fill back to `fillCard`, so the one moment
    // a person is told their press landed said nothing at all.
    readonly property color fillCardPressed: highContrast ? fromPalette("window", systemPalette.window)
                                           : onDarkSurface ? "#08FFFFFF" : "#4DF9F9F9"
    // LayerFillColorDefault: the quiet surface content sits on above Mica —
    // the rail and the messages bar. One step below a card, on purpose.
    readonly property color fillLayer: highContrast ? fromPalette("window", systemPalette.window)
                                     : onDarkSurface ? "#4C3A3A3A" : "#80FFFFFF"
    // DividerStrokeColorDefault, for a rule between things on one surface.
    readonly property color strokeDivider: highContrast ? fromPalette("windowText", systemPalette.windowText)
                                         : onDarkSurface ? "#15FFFFFF" : "#0F000000"
    // SubtleFillColorSecondary: a chip, a skeleton, a track.
    readonly property color fillSubtle: highContrast ? fromPalette("window", systemPalette.window)
                                      : onDarkSurface ? "#0FFFFFFF" : "#09000000"

    // ---- The lit tile -----------------------------------------------------
    //
    // The organization's initial, and the badge of an empty state, sit on a
    // tile of the person's own accent colour lit from one side — the same
    // light that falls across the pictures on the machine cards
    // (`MachineArt.qml`), so the window has one source of it.
    //
    // **Always a deep colour, in both themes, because white sits on it.** The
    // palette's accent is dark on a light desktop and *light* on a dark one
    // (Windows hands dark mode SystemAccentColorLight2), and a white initial
    // on that is unreadable. So the tile is built from the accent's hue at a
    // fixed, low lightness rather than from the accent itself, and its second
    // stop is the first turned a seventh of the way round the wheel.
    function deep(c, lightness, turn) {
        var h = (c.hslHue < 0 ? 0.58 : c.hslHue) + turn
        return Qt.hsla(h - Math.floor(h), Math.min(0.85, Math.max(0.45, c.hslSaturation)), lightness, 1)
    }
    readonly property color tileFrom: highContrast ? fromPalette("highlight", systemPalette.highlight) : deep(accent, 0.42, 0)
    readonly property color tileTo: highContrast ? fromPalette("highlight", systemPalette.highlight) : deep(accent, 0.34, 0.14)
    // What sits on the tile. `highlightedText` is the palette's promise about
    // exactly this pair; white is only right because the tile is always deep.
    readonly property color onTile: highContrast ? fromPalette("highlightedText", systemPalette.highlightedText) : "white"

    // The four system fill colours a status is painted with — see above.
    //
    // **All four collapse to one under high contrast**, which is what
    // Microsoft's own HighContrast dictionary does (it sets every
    // SystemFillColor to the same value): colour stops carrying meaning
    // there, and the word beside it is the only thing left. Every consumer of
    // these draws that word, which is why the collapse costs nothing.
    readonly property color fillSuccess: highContrast ? fromPalette("windowText", systemPalette.windowText)
                                       : onDarkSurface ? "#6CCB5F" : "#0F7B0F"
    readonly property color fillCaution: highContrast ? fromPalette("windowText", systemPalette.windowText)
                                       : onDarkSurface ? "#FCE100" : "#9D5D00"
    readonly property color fillCritical: highContrast ? fromPalette("windowText", systemPalette.windowText)
                                        : onDarkSurface ? "#FF99A4" : "#C42B1C"
    readonly property color fillNeutral: highContrast ? fromPalette("windowText", systemPalette.windowText)
                                       : onDarkSurface ? "#8BFFFFFF" : "#72000000"

    // ---- Icons ----------------------------------------------------------
    //
    // Segoe Fluent Icons, by name, so that a view says `Theme.icon.refresh`.
    // **A view never carries the code point itself.** A private-use character
    // pasted into a QML file is invisible to whoever reviews the diff, is not
    // greppable, and cannot be told apart from the next one along; the name is
    // the only part of it a person can check. `.github/workflows/
    // omnuv-change-budget.yml` fails the build if one appears outside this
    // file.
    //
    // Each name below is Microsoft's own name for that glyph, kept verbatim in
    // the comment where ours differs, because the documentation is indexed by
    // theirs. Note the two that are easy to get wrong: it is `Setting`, not
    // "Settings"; and the back arrow has two of them — `ChromeBack` for a
    // title bar, `Back` for a content one.
    //
    // **Windows 10 does not ship Segoe Fluent Icons**, and an absent family
    // renders boxes, which breaks that platform as surely as a failed API call
    // would. `Segoe MDL2 Assets` does ship with Windows 10 and holds every one
    // of these glyphs at the same code point, so the fallback costs a second
    // family name and no glyph table of its own. Consumers set
    // `font.family: Theme.iconFamily`, chosen the same way as `textFamily`.
    readonly property var iconFamilies: ["Segoe Fluent Icons", "Segoe MDL2 Assets"]
    readonly property string iconFamily: firstInstalled(iconFamilies)
    // Whether either icon font is actually here. `firstInstalled` falls back
    // to the last name whether or not it exists, so on a Linux or macOS
    // desktop a glyph-only button draws an empty box; a button that is only a
    // glyph asks this and shows its word instead.
    readonly property bool iconsInstalled: Qt.fontFamilies().indexOf(iconFamily) >= 0

    readonly property QtObject icon: QtObject {
        readonly property string settings: "\uE713"      // Setting
        readonly property string refresh: "\uE72C"       // Refresh
        readonly property string play: "\uE768"          // Play
        readonly property string more: "\uE712"          // More
        readonly property string accept: "\uE8FB"        // Accept
        readonly property string dismiss: "\uE711"       // Cancel
        readonly property string back: "\uE72B"          // Back, in content
        readonly property string titleBarBack: "\uE830"  // ChromeBack, in a title bar
        readonly property string windowClose: "\uE8BB"   // ChromeClose
        readonly property string quit: "\uE7E8"          // PowerButton
        readonly property string info: "\uE946"          // Info
        readonly property string warning: "\uE7BA"       // Warning
        readonly property string error: "\uE783"         // Error
        readonly property string game: "\uE7FC"          // Game
        readonly property string terminal: "\uE756"      // CommandPrompt
        readonly property string network: "\uE968"       // Network
        readonly property string globe: "\uE774"         // Globe
        readonly property string key: "\uE8D7"           // Permissions
        readonly property string spend: "\uE8C7"         // PaymentCard
        readonly property string history: "\uE81C"       // History
        readonly property string chevronUp: "\uE70E"     // ChevronUp
        readonly property string chevronDown: "\uE70D"   // ChevronDown
        readonly property string cloud: "\uE753"         // Cloud
        readonly property string waiting: "\uE823"       // Recent
        readonly property string message: "\uE8BD"       // Message
        readonly property string completed: "\uE930"     // Completed
        readonly property string sync: "\uE895"          // Sync
        readonly property string ring: "\uEA3A"          // CircleRing
    }
}
