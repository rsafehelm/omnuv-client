#include "appearance.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QQuickStyle>
#include <QWindow>

#ifdef Q_OS_WIN
// Qt first, <windows.h> last: it defines `min` and `max` as macros, and a
// Qt header parsed after it is how that becomes somebody else's afternoon.
// <dwmapi.h> after <windows.h> in turn, because it is written expecting the
// types <windows.h> brings in and does not include them itself.
#include <QSettings>
#include <windows.h>
#include <dwmapi.h>
#else
#include <QStyleHints>
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

bool queryDark()
{
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

}

OmnuvAppearance::OmnuvAppearance(QObject* parent)
    : QObject(parent),
      m_animations(queryAnimations()),
      m_dark(queryDark())
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

void OmnuvAppearance::refresh()
{
    const bool animations = queryAnimations();
    const bool dark = queryDark();
    if (animations == m_animations && dark == m_dark) {
        return;
    }

    m_animations = animations;
    m_dark = dark;
    announce();
    emit changed();
}

void OmnuvAppearance::announce() const
{
    qInfo("Omnuv appearance: motion=%s theme=%s",
          m_animations ? "on" : "off",
          m_dark ? "dark" : "light");
}

void OmnuvAppearance::applyStyle()
{
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
    QQuickStyle::setStyle(QStringLiteral("FluentWinUI3"));
#else
    // Upstream's Material, unchanged, everywhere else. Not a fallback: on a Mac
    // or a Linux desktop FluentWinUI3 would be the foreign look, which is the
    // thing this change exists to stop doing on Windows.
    QQuickStyle::setStyle(QStringLiteral("Material"));
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
