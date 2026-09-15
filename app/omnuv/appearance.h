#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

// What the operating system has asked every application to look like, and to
// move like.
//
// Two settings, read from the system rather than guessed, and re-read whenever
// the system says they changed:
//
//   animationsEnabled   the person has not asked for reduced motion
//   darkAppsTheme       applications should draw themselves dark
//
// **Neither is a preference of ours.** There is no switch in this application
// for either, deliberately: both already have one in the operating system's own
// settings, and a second switch somewhere else is how an application ends up
// disagreeing with the desktop it is sitting on. We read, we follow, we say in
// the log what we read.
//
// The first is an accessibility setting before it is a taste: Microsoft's own
// description of it says that "flashing, blinking, flickering, and moving
// content can cause seizures in users with photo-sensitive epilepsy". An
// animation this object reports as unwanted is not a nicety to be traded off
// against how the view looks.
//
// Exposed to QML as `Omnuv.appearance` — a property of the session rather than
// a singleton of its own, because a singleton is only constructed when some QML
// file first mentions it, and this object has to have read and logged its two
// values whether or not anything is looking. The session is resolved on the
// first line of `main.qml`, so being one of its members is what makes the log
// line certain.
class OmnuvAppearance : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

    // One `changed()` for both. They are read together, they are re-read
    // together by the one message the system sends, and a QML binding that
    // re-evaluates because the other one moved costs nothing. Two signals would
    // be two things to keep in step for no gain.
    Q_PROPERTY(bool animationsEnabled READ animationsEnabled NOTIFY changed)
    Q_PROPERTY(bool darkAppsTheme READ darkAppsTheme NOTIFY changed)

    // **A third value, and not a duplicate of the second.** Windows has two
    // theme switches and honours both: `AppsUseLightTheme` decides how an
    // application draws itself, and `SystemUsesLightTheme` decides the taskbar,
    // the Start menu and the notification area. A person can and often does
    // set them differently — light applications on a dark taskbar is the
    // Windows 11 default the first time somebody turns dark mode half on.
    //
    // The tray icon is painted *onto the taskbar*, so it is the only thing in
    // this application that follows the second one. Everything drawn inside our
    // own window follows the first. Reading one for the other is how a
    // monochrome icon ends up dark-on-dark and invisible.
    Q_PROPERTY(bool darkSystemTheme READ darkSystemTheme NOTIFY changed)

public:
    explicit OmnuvAppearance(QObject* parent = nullptr);
    ~OmnuvAppearance() override;

    // The Windows 11 look, in two halves, because they happen at two very
    // different moments and only one of them can wait for this object to exist.
    //
    // `applyStyle()` chooses the Qt Quick Controls style. It has to run before
    // any QML that imports Qt Quick Controls is loaded — `QQuickStyle::setStyle`
    // checks for the registered module and refuses with a warning once it is
    // there — so it is static, and it is the one call of ours that `app/main.cpp`
    // makes, because the application object is constructed there and nothing of
    // ours runs earlier.
    //
    // `applyBackdrop()` asks the Desktop Window Manager for Mica behind the
    // window. That needs a native window handle, so it waits for one: it is
    // called each time the window becomes visible, and does nothing on a handle
    // it has already asked about.
    static void applyStyle();
    void applyBackdrop();

    bool animationsEnabled() const { return m_animations; }
    bool darkAppsTheme() const { return m_dark; }
    bool darkSystemTheme() const { return m_darkShell; }

    // The same reading, without an instance, asked now rather than recalled.
    //
    // Two reasons it is not `animationsEnabled()` on the live object. The
    // object refreshes on WM_SETTINGCHANGE, which arrives through Qt's event
    // loop -- and `app/streaming/session.cpp` suspends Qt for the whole of a
    // stream, so during the one hour anybody is actually playing, the cached
    // value is the one read at startup and cannot move. And there is no
    // instance reachable from the streaming code, which is under SDL's main
    // thread rather than under the QML object graph.
    //
    // It calls the same query the object calls, so there is one definition of
    // what the setting is and where it comes from. A system call per second is
    // not a cost worth caching against.
    static bool animationsEnabledNow();

    // Windows broadcasts WM_SETTINGCHANGE to every top-level window when either
    // of these is changed, and this is how a Qt application sees it. Returns
    // false always: the message is for everybody, and consuming it would take
    // it away from Qt's own theme handling in the same process.
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void changed();

private:
    // Re-read both, and speak only if something moved. Cheap enough to call on
    // every WM_SETTINGCHANGE — which arrives for changes that have nothing to
    // do with us, so "nothing moved" is the normal case.
    void refresh();

    // The one greppable line, written at startup and again whenever a value
    // changes: `motion=on|off theme=dark|light taskbar=dark|light`. The lab
    // rig's report asserts on it, so its shape is part of the contract with
    // `lab-windows-build.yml` rather than a debugging aid to be reworded
    // freely. The third token was **appended** rather than inserted for that
    // reason: `omnuv-run.ps1` captures `theme=(\S+)`, which stops at the
    // space, so a token added at the end costs nothing and a token added in
    // the middle would have moved what that pattern captures.
    void announce() const;

    bool m_animations;
    bool m_dark;
    bool m_darkShell;

    // The native window Mica was last requested for, as a WId rather than an
    // HWND so this header stays free of <windows.h>. Zero until the window
    // exists. Keyed on the handle rather than on a bool because Qt destroys and
    // recreates a native window on some flag changes, and a window attribute
    // dies with the handle it was set on.
    quintptr m_backdrop = 0;
};
