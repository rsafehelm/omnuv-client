#include "appearance.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QAccessibilityHints>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStyleHints>
#include <QWindow>

#ifdef Q_OS_WIN
// Qt first, <windows.h> last: it defines `min` and `max` as macros, and a
// Qt header parsed after it is how that becomes somebody else's afternoon.
// <dwmapi.h> after <windows.h> in turn, because it is written expecting the
// types <windows.h> brings in and does not include them itself.
#include <QSettings>
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {

bool queryAnimations()
{
#ifdef Q_OS_WIN
    // SPI_GETCLIENTAREAANIMATION, 0x1042. Documented as: "Determines whether
    // animations are enabled or disabled. The pvParam parameter must point to a
    // BOOL variable that receives TRUE if animations are enabled, or FALSE
    // otherwise." uiParam is unused and passed as zero; fWinIni is only
    // meaningful for the SPI_SET* half and is zero here.
    //
    // Not supported before Vista, which we do not build for.
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
        // The call leaves `enabled` alone on failure, so the initialiser above
        // is the answer. It is TRUE rather than FALSE because that is what this
        // application did before it asked at all: an unreadable setting should
        // change nothing, and a client that silently froze every transition
        // would look broken rather than considerate.
        qWarning("Omnuv: SPI_GETCLIENTAREAANIMATION failed (%lu)", GetLastError());
        return true;
    }
    return enabled != FALSE;
#else
    // macOS has `accessibilityDisplayShouldReduceMotion` and GNOME has
    // `org.gnome.desktop.interface enable-animations`; Qt surfaces neither, and
    // reaching for AppKit or gsettings is work this phase does not need — the
    // rig this is for is Windows. `true` is the honest answer because it is
    // exactly what every one of these platforms does today: nothing anywhere in
    // this application asks before animating.
    return true;
#endif
}

// `OMNUV_THEME=light|dark`, the sibling of `OMNUV_STYLE` below and the same
// kind of thing: a debugging switch so the rig can photograph both themes
// without anybody changing its Windows settings. Anything else, including
// unset, is "ask the system". **It works where the platform theme honours
// `requestColorScheme()`** — Windows and macOS do; Qt's generic Unix theme
// does not, so the Linux loop draws dark whatever this says.
int forcedTheme()
{
    const QByteArray forced = qgetenv("OMNUV_THEME");
    return forced == "dark" ? 1 : forced == "light" ? 0 : -1;
}

// `OMNUV_CONTRAST=on|off`, the sibling of `OMNUV_THEME`: high contrast cannot
// be turned on from a program — Windows' own `SPI_SETHIGHCONTRAST` is
// documented as unsupported and the supported way is applying a theme file to
// the logged-on user — so this is how the loops photograph what our own
// drawing does under it. It forces *our* decoration off; it cannot make the
// style's palette a high-contrast one, and the picture has to be read knowing
// that.
bool queryContrast()
{
    const QByteArray forced = qgetenv("OMNUV_CONTRAST");
    if (forced == "on") {
        return true;
    }
    if (forced == "off") {
        return false;
    }
    return QGuiApplication::styleHints()->accessibility()->contrastPreference()
           == Qt::ContrastPreference::HighContrast;
}

bool queryDark()
{
    if (forcedTheme() >= 0) {
        return forcedTheme() == 1;
    }
#ifdef Q_OS_WIN
    // HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\
    // Personalize, value `AppsUseLightTheme`, a DWORD, where 0 means dark.
    //
    // **Read here rather than taken from Qt, and the reason is specific.**
    // `QStyleHints::colorScheme()` reads this exact key on Windows — but it
    // first asks whether the platform plugin was told to handle dark mode at
    // all, and answers `Qt::ColorScheme::Light` when it was not, whatever the
    // registry says. That is Qt reporting how *it* intends to draw. What every
    // later phase needs is what the *person* asked the operating system for, so
    // we ask the operating system.
    //
    // Absent on a machine whose theme has never been changed, so the default is
    // 1 — light, which is what Windows itself defaults to.
    QSettings personalize(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return personalize.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
#else
    // Qt has a real answer here and it is better than anything we would write:
    // one property backed by each platform's own notion, since Qt 6.5, and this
    // fork builds against 6.11. `Unknown` — which is what Qt reports under a
    // high-contrast theme, and on a platform that has no such setting — is not
    // dark, and treating it as light is the same choice Qt makes.
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#endif
}

bool queryDarkShell()
{
#ifdef Q_OS_WIN
    // The same key as `queryDark()`, a different value: `SystemUsesLightTheme`,
    // a DWORD, 0 meaning dark. This is the one the shell paints the taskbar,
    // the Start menu and the notification area from, and therefore the one a
    // tray icon has to match — an icon drawn for the *apps* theme is
    // dark-on-dark and invisible on every machine where the two differ.
    //
    // Absent before Windows 10 1903, which is when Microsoft split the setting
    // in two. The default is 1 — light — because that is what a Windows whose
    // theme has never been touched draws, and it is also the safer of the two
    // to be wrong about: a dark glyph on a dark taskbar disappears, while a
    // light one on a light taskbar is merely faint.
    QSettings personalize(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return personalize.value(QStringLiteral("SystemUsesLightTheme"), 1).toInt() == 0;
#else
    // No second setting to read. Everywhere else the tray icon and the window
    // follow the one theme the desktop has.
    return queryDark();
#endif
}

}

OmnuvAppearance::OmnuvAppearance(QObject* parent)
    : QObject(parent),
      m_animations(queryAnimations()),
      m_dark(queryDark()),
      m_darkShell(queryDarkShell()),
      m_contrast(queryContrast())
{
    announce();

#ifdef Q_OS_WIN
    // Installed on the application object, which exists by now: this is built
    // with the session, and the session is built when the QML engine first
    // resolves it — long after `QApplication` is constructed.
    QCoreApplication::instance()->installNativeEventFilter(this);
#else
    // The platforms where we did not read the theme ourselves get their change
    // notification from the same place the value came from. Not connected on
    // Windows, because there the signal is downstream of the gate described in
    // `queryDark()` and would stay silent while the registry moved.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, &OmnuvAppearance::refresh);
#endif

    // High contrast is Qt's to report on every platform — it has its own
    // signal and does not come through `WM_SETTINGCHANGE`'s handler above, so
    // it is connected on Windows too. Turning it on mid-session is exactly
    // what somebody who needs it does.
    connect(QGuiApplication::styleHints()->accessibility(),
            &QAccessibilityHints::contrastPreferenceChanged,
            this, &OmnuvAppearance::refresh);
}

OmnuvAppearance::~OmnuvAppearance()
{
#ifdef Q_OS_WIN
    // This object outlives nothing in practice — it dies with the session at
    // exit — but a filter left installed on a destroyed object is a crash in
    // the one shutdown ordering where it does not.
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
#endif
}

bool OmnuvAppearance::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)

#ifdef Q_OS_WIN
    // Qt hands us "windows_generic_MSG" for a message sent to a top-level
    // window and "windows_dispatcher_MSG" for a system-wide one, and documents
    // that in both cases `message` is a MSG. A broadcast arrives as the first,
    // but we have no reason to care which, so we do not read the string.
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message == WM_SETTINGCHANGE) {
        // **Both values, on every one of these, without looking at wParam.**
        // The two settings announce themselves differently — the animation one
        // as `wParam == SPI_SETCLIENTAREAANIMATION`, the theme one as `lParam`
        // pointing at the string "ImmersiveColorSet" — and Microsoft's own
        // advice for this message is the general form of that: "In general,
        // when you receive this message, you should check and reload any
        // system parameter settings that are used by your application."
        //
        // Two registry-shaped reads on a message that arrives a handful of
        // times in a session is not a cost worth a switch statement that has to
        // be right about two different encodings.
        refresh();
    }
#else
    Q_UNUSED(message)
#endif

    // Never consumed. Qt's own theme handling lives on the other side of this
    // filter in the same process, and swallowing the broadcast would leave the
    // application following the desktop while Qt did not.
    return false;
}

bool OmnuvAppearance::animationsEnabledNow()
{
    // The same `queryAnimations()` the instance uses -- one definition of the
    // setting, asked fresh. See the comment on the declaration for why a
    // stream cannot use the cached value.
    return queryAnimations();
}

void OmnuvAppearance::refresh()
{
    const bool animations = queryAnimations();
    const bool dark = queryDark();
    const bool darkShell = queryDarkShell();
    const bool contrast = queryContrast();
    if (animations == m_animations && dark == m_dark && darkShell == m_darkShell
        && contrast == m_contrast) {
        return;
    }

    m_animations = animations;
    m_dark = dark;
    m_darkShell = darkShell;
    m_contrast = contrast;
    announce();
    emit changed();
}

void OmnuvAppearance::announce() const
{
    qInfo("Omnuv appearance: motion=%s theme=%s taskbar=%s contrast=%s",
          m_animations ? "on" : "off",
          m_dark ? "dark" : "light",
          m_darkShell ? "dark" : "light",
          m_contrast ? "high" : "normal");
}

// **Mica needs a window that can be seen through, and Qt decides that before
// the window exists.** A Quick window gets an alpha channel only if the
// default was set before the first one is created; after that it is too late.
//
// **Off unless `OMNUV_MICA` asks for it, and that is a measurement rather than
// caution.** Every link was proved on rig 9100 on 17 September — the attribute
// accepted (`MICA=requested`), transparency effects on (`TRANSPARENCY=on`), a
// hardware renderer with an alpha channel, the frame extended, the window's own
// colour gone (`BACKDROP=shown`) — and DWM drew **flat white** behind it
// instead of Mica. That rig's desktop runs on a Microsoft Basic Display
// Adapter, because its only real GPU is passed through with no monitor, and
// DWM does not draw backdrop material on one.
//
// Nothing in Windows reports that refusal: `DwmGetWindowAttribute` reads back
// the value we set, and the documented list of conditions under which the
// system silently substitutes a flat fill is aggregated nowhere. So a client
// that turned this on by default would, on any machine DWM refuses, replace a
// correct `#F3F3F3` window with a white one and have no way to notice. On
// until somebody has seen it work:
//
//     OMNUV_MICA=on      alpha, the frame extended, the window transparent
//     OMNUV_MICA=plain   the same without `DwmExtendFrameIntoClientArea`,
//                        for telling the two apart on a machine where one
//                        works and the other does not
//     unset or anything else   what every build before this did
//
// Never under the software backend either: there a translucent window is a
// layered GDI window, which DWM composes against the *desktop* rather than
// against its own backdrop — the person would see their wallpaper through the
// application rather than a blurred tint of it.
bool OmnuvAppearance::wantsAlpha()
{
#ifdef Q_OS_WIN
    const QByteArray want = qgetenv("OMNUV_MICA");
    return (want == "on" || want == "plain")
           && QQuickWindow::sceneGraphBackend() != QLatin1String("software");
#else
    return false;
#endif
}

void OmnuvAppearance::applyStyle()
{
    if (wantsAlpha()) {
        QQuickWindow::setDefaultAlphaBuffer(true);
    }

    // `OMNUV_STYLE` wins on every platform, and it exists for one reason: an
    // explicit `setStyle` beats `QT_QUICK_CONTROLS_STYLE`, so without it the
    // Linux loop (`scripts/client-linux`, removed from the omnuv repo on
    // 21 September 2026) could only ever draw Material — and
    // the defects that cost 15 September were FluentWinUI3's sizing under the
    // software backend, which runs on Linux just as well. A debugging switch,
    // not a preference: nothing sets it in a shipped configuration.
    // The theme first, because the style reads it as it loads. Qt draws the
    // controls from `QStyleHints::colorScheme()`, so that is what is set; and
    // `queryDark()` answers the same, so `Theme` and the style cannot disagree.
    if (forcedTheme() >= 0) {
        QGuiApplication::styleHints()->setColorScheme(forcedTheme() == 1 ? Qt::ColorScheme::Dark
                                                                       : Qt::ColorScheme::Light);
    }

    const QString forced = qEnvironmentVariable("OMNUV_STYLE");
    if (!forced.isEmpty()) {
        QQuickStyle::setStyle(forced);
    } else
#ifdef Q_OS_WIN
    // Qt's own Fluent/WinUI 3 style, which draws Qt Quick Controls the way
    // Windows 11 draws its own: the 4px control corners, the system accent, and
    // light or dark taken from the same setting `queryDark()` above reads.
    //
    // Built in since Qt 6.8 and this fork pins 6.11.2, so it is present rather
    // than hoped for: `QQuickStylePrivate::builtInStyles()` lists it with no
    // platform guard, and the build feature behind it
    // (`quickcontrols2-fluentwinui3`) is conditioned only on Fusion's, which
    // every official desktop build has. It ships inside qtdeclarative, which is
    // in the base archive set the rig's `aqt install-qt` fetches.
    //
    // A handful of containers have no Fluent implementation and fall back to
    // Fusion — `StackView`, which `main.qml` uses, among them. That is
    // documented and accepted: a StackView draws nothing of its own, and the
    // buttons, fields, menus and dialogs inside it are what carry the look.
    {
        QQuickStyle::setStyle(QStringLiteral("FluentWinUI3"));
    }
#else
    // Upstream's Material, unchanged, everywhere else. Not a fallback: on a Mac
    // or a Linux desktop FluentWinUI3 would be the foreign look, which is the
    // thing this change exists to stop doing on Windows.
    {
        QQuickStyle::setStyle(QStringLiteral("Material"));
    }
#endif

    // One greppable line, in the same family as the `motion=`/`theme=` one
    // above, because a style is otherwise invisible in a log.
    //
    // **It reports what was asked for, not what resolved**, and that is a
    // property of Qt rather than a shortcut taken here: `QQuickStyle::name()`
    // returns the string `setStyle()` was handed, so it would echo a
    // misspelling happily. What actually catches a wrong name is louder and is
    // already in `main.cpp` — since Qt 6.5 a style is a QML module, an unknown
    // name is treated as a *custom* style and imported under that name, the
    // import fails, `engine.rootObjects()` comes back empty, and main.cpp turns
    // that into `return -1`. A window existing at all is the proof the style
    // loaded; this line is what says which one was asked for.
    qInfo("Omnuv appearance: style=%s", qPrintable(QQuickStyle::name()));
}

void OmnuvAppearance::applyBackdrop()
{
#ifdef Q_OS_WIN
    // The application's own window, found rather than held: this object is
    // built with the session, which the QML engine resolves before it creates
    // the window, so a pointer taken at construction would be null. `Qt::Window`
    // is what excludes the tray menu's popup and every tooltip, which are also
    // top-level windows and are sometimes the visible one.
    QWindow* window = nullptr;
    const auto windows = QGuiApplication::topLevelWindows();
    for (QWindow* w : windows) {
        if (w->type() == Qt::Window) {
            window = w;
            break;
        }
    }
    if (window == nullptr) {
        return;
    }

    // Creates the native window if it does not exist yet, and answers 0 when the
    // platform refused to make one. Nothing to hand DWM in that case, and
    // nothing we could do about it either.
    const WId id = window->winId();
    if (id == 0) {
        return;
    }

    // Microsoft's guidance is "don't apply backdrop material more than once in
    // an application", and this is called every time the window becomes
    // visible. Keyed on the handle rather than on a flag so that a native
    // window Qt destroyed and recreated is asked again — a window attribute
    // dies with the HWND it was set on.
    if (m_backdrop == id) {
        return;
    }
    m_backdrop = id;

    // `WId` is `quintptr`, not `HWND`: qwindowdefs.h typedefs it to an integer
    // so the header stays platform-neutral, and only forward-declares the
    // handle types. The cast back is ours to make.
    const HWND hwnd = reinterpret_cast<HWND>(id);

    // `DWMWA_SYSTEMBACKDROP_TYPE` (38) and `DWMSBT_MAINWINDOW` (2), used as the
    // SDK's own symbols rather than written as numbers. Both are plain
    // enumerators in `dwmapi.h` with no version guard around them, so no
    // preprocessor test can detect their absence: an SDK older than the Windows
    // 11 22621 one the rig installs fails to compile here, loudly, which is the
    // right way round — quietly sending 38 to a DWM that means something else
    // by it would not be.
    //
    // MAINWINDOW rather than AUTO: AUTO draws the material only behind the
    // default Win32 title bar and may decide to draw nothing at all, while
    // MAINWINDOW covers the whole window including the non-client area. That is
    // the one that reads as a Windows 11 application rather than as a Windows
    // 11 title bar on something older.
    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
    const HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                             &backdrop, sizeof(backdrop));

    // The build number, because it is the first thing anybody reading "why is
    // it flat" wants and it costs one call. Qt takes it from `RtlGetVersion`,
    // which reports the real version rather than the 6.2 that `GetVersionEx`
    // hands an application whose manifest does not claim Windows 10. 22621 is
    // the floor Microsoft documents for this attribute.
    const int build = QOperatingSystemVersion::current().microVersion();

    if (SUCCEEDED(hr)) {
        // **`requested`, not `applied`, and the word is carrying real weight.**
        // Nothing in Windows reports whether Mica is on the screen.
        // `DwmGetWindowAttribute` reads back the value we set, not what DWM
        // drew; `MicaController.IsSupported` answers *supported* and belongs to
        // a Windows App SDK this fork will not acquire for one boolean; and the
        // documented list of conditions under which the system silently
        // substitutes a flat fill — transparency turned off in Settings,
        // battery saver, modest hardware, the window losing focus, and
        // "internal heuristics" Microsoft reserves the right to add to — is
        // aggregated nowhere at all. S_OK means the attribute was accepted. It
        // does not mean anyone can see it, so `applied` would be a claim this
        // process has no way to support.
        //
        // And it is not visible yet for a reason of our own: the window's
        // background is still painted opaque, which is the next step rather
        // than this one. Mica only shows through layers that are transparent.
        qInfo("Omnuv appearance: mica=requested hr=0x%08lx build=%d",
              static_cast<unsigned long>(hr), build);

        // **And then the layer that was in the way.** The attribute above tells
        // DWM to draw Mica behind this window; it does nothing while the window
        // goes on painting an opaque background over it. Three things are
        // checked rather than assumed: it is a Quick window; the renderer that
        // actually came up is not the software one, because Qt falls back to it
        // without a word when Direct3D cannot start; and the surface really has
        // the alpha channel `wantsAlpha()` asked for. The view does the last
        // step — it reads `backdrop` and makes the window's colour transparent.
        //
        // `DwmExtendFrameIntoClientArea` with -1 margins is what puts the
        // backdrop behind the *whole* client area rather than behind the title
        // bar alone — see `wantsAlpha()` for the switch and for what the rig
        // measured.
        auto* quick = qobject_cast<QQuickWindow*>(window);
        const bool hardware = quick != nullptr && quick->rendererInterface() != nullptr
            && quick->rendererInterface()->graphicsApi() != QSGRendererInterface::Software;
        const bool alpha = quick != nullptr && quick->format().alphaBufferSize() > 0;
        bool shown = hardware && alpha;
        HRESULT frame = S_OK;
        if (shown && qgetenv("OMNUV_MICA") != "plain") {
            const MARGINS glass = { -1, -1, -1, -1 };
            frame = DwmExtendFrameIntoClientArea(hwnd, &glass);
            shown = SUCCEEDED(frame);
        }
        qInfo("Omnuv appearance: backdrop=%s hardware=%d alpha=%d frame=0x%08lx",
              shown ? "shown" : "opaque", hardware ? 1 : 0, alpha ? 1 : 0,
              static_cast<unsigned long>(frame));
        if (shown != m_backdropShown) {
            m_backdropShown = shown;
            emit changed();
        }
    } else {
        // Windows 10 lands here, and that is the specification rather than a
        // disappointment: the attribute is refused, DWM draws nothing behind
        // the window, and the flat surface it already had stays exactly as it
        // was. Branching on `FAILED` rather than on a particular error code on
        // purpose — Microsoft's reference documentation says only that the
        // function returns an HRESULT and never names the one an unrecognised
        // attribute produces, so the value is logged rather than compared.
        qInfo("Omnuv appearance: mica=unavailable:0x%08lx build=%d",
              static_cast<unsigned long>(hr), build);
    }
#endif
}
