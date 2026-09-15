#include "appearance.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
// Qt first, <windows.h> last: it defines `min` and `max` as macros, and a
// Qt header parsed after it is how that becomes somebody else's afternoon.
#include <QSettings>
#include <windows.h>
#else
#include <QGuiApplication>
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
