#pragma once

#include <QDateTime>
#include <QIcon>
#include <QList>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>

#include "traystate.h"

class QMenu;
class QAction;
class QTimer;
class OmnuvSession;
class OmnuvAutostart;
class OmnuvProbe;

// The tray, which is the whole of being a widget.
//
// A widget is a program that was already running when you needed it.
// Everything else in this application already works; what was missing is that
// it had to be opened before it knew anything.
//
// **C++ and QSystemTrayIcon, not QML and Qt.labs.platform, and that was
// learned the expensive way.** The first version of this was a QML file
// importing `Qt.labs.platform`. It compiled, it linked, it deployed, and then
// every Windows build — CI's and the lab rig's alike — exited a second after
// launch with no window and no error on screen:
//
//     QQmlApplicationEngine failed to load component
//     qrc:/gui/main.qml: Type OmnuvTray unavailable
//     qrc:/omnuv/OmnuvTray.qml: module "Qt.labs.platform" is not installed
//
// The module was present in the Qt installation and absent from the deployed
// application, because `scripts/build-arch.bat` runs
// `windeployqt --qmldir app\gui` — it scans *that one directory* to decide
// which QML modules to ship, and our QML lives in `app/omnuv`. Nothing we
// could put in our own files would have told it otherwise, and the batch file
// belongs to upstream.
//
// So: no QML module to deploy, nothing for a scanner to miss, and a stable API
// instead of a labs one whose contract is explicitly provisional. It needs
// QApplication rather than QGuiApplication — which is the same change the tray
// already required, so the change budget grows by nothing.
//
// ---------------------------------------------------------------------------
//
// **The menu is the dashboard now, and the rule it used to carry is gone on
// purpose.** This header said, until today, that a tray menu is not a
// dashboard, and gave a reason worth repeating: machine rows here would be a
// second place where the machine list is *interpreted*, and the client
// displays while Core decides.
//
// The reason survives. The rule does not, and the two came apart:
//
//   * The sentence was written for W1's five-item menu and the queue marks it
//     "superseded text" where it appears; the design it belongs to
//     (`docs/client-widget.md`'s `Qt.labs.platform` tray) is the same design
//     this file's own header reverses two paragraphs above. It is not a
//     standing decision that I3 overrode — it is older text about a shipped
//     scope.
//
//   * D1 and `windows_impl.md` I3 are the later and more specific word, and
//     what they ask for is not interpretation. A row is a name Core sent, the
//     status word Core chose, and a dot keyed on that word by the *same*
//     grouping `MachineCard.qml` already uses — pinned against it in
//     `.github/workflows/omnuv-change-budget.yml`, so there is exactly one
//     grouping and the build fails if a second appears.
//
// What is still refused, because that is the half that was load-bearing: no
// vocabulary of this application's own, nothing derived from a field Core did
// not send, no row that says *why* beyond `last_error` and `waiting_on`
// verbatim, and no aggregate — "2 of 3 sites up" is provider data wearing a
// count. A buyer sees their own machines and their own device, and nothing
// else exists as far as this menu is concerned.
class OmnuvTray : public QObject
{
    Q_OBJECT

public:
    // Always constructs, and the object then says for itself whether the
    // shell took the icon — see `checkRegistered`.
    //
    // This used to return nullptr when `isSystemTrayAvailable()` said no,
    // which threw away the case Qt documents as ordinary: *"if the system
    // tray is currently unavailable but becomes available later,
    // QSystemTrayIcon will automatically add an entry in the system tray if
    // it is visible"*. Deciding once, at construction, meant a program
    // started at login — before the shell has finished building the
    // notification area — had no tray for the rest of that session, and said
    // nothing about it either. The decision is Qt's to make, continuously;
    // ours is only to report it.
    static OmnuvTray* create(OmnuvSession* session, QObject* parent = nullptr);

private slots:
    void refresh();
    void checkRegistered();
    void openWindow();
    void showAbout();
    void activated(QSystemTrayIcon::ActivationReason reason);
    void toggleAutostart(bool on);

    // Take the local readings again. On its own timer rather than on the
    // window's: see `kLocalPollMs` in the implementation for why the window's
    // was not enough.
    void probeLocal();

    // One frame of the connecting ring.
    void spin();

private:
    explicit OmnuvTray(OmnuvSession* session, QObject* parent);

    // Everything the icon is allowed to look at, gathered from the live
    // objects. The decision itself is `omnuv::iconFor` in `traystate.h`, which
    // has no Qt in it so that the exit check can run it.
    omnuv::Readings readings() const;

    // The headline: one line, Omnuv's own state, never a machine's.
    QString stateLine() const;

    void rebuildMachines();
    // `replacement`, when non-empty, is a word that stands in for pass / fail
    // / unknown — for the one row state that is none of the three.
    void setRow(QAction* row, const QString& label, omnuv::Reading reading,
                const QDateTime& takenAt, const QString& replacement);
    void applyIcon();

    // A monochrome glyph for one state, at every size the shell asks for.
    QIcon glyph(omnuv::TrayIcon state, int frame) const;

    // Send one, and say that we sent it — including what the shell was
    // willing to show at the time, because a toast that never appeared and a
    // change that never happened are the same silence in a log that only
    // records the change.
    void toast(const char* kind, const QString& title, const QString& body);

    OmnuvSession* m_session;
    OmnuvProbe* m_probe;
    QSystemTrayIcon* m_icon;
    QMenu* m_menu;
    QAction* m_state;
    QAction* m_machinesHeader;
    QAction* m_deviceAnchor;   // machine rows are inserted before this
    QAction* m_rowInternet;
    QAction* m_rowOmnuv;
    QAction* m_rowNetwork;
    QAction* m_autostart;
    OmnuvAutostart* m_auto;

    QList<QAction*> m_machineRows;

    // Re-reads the local probes whether or not any window is open.
    QTimer* m_localPoll;

    // Only alive while the icon is the connecting one, and never under
    // reduced motion.
    QTimer* m_spinner;
    int m_frame;

    // What is currently painted, so an unchanged state does not repaint — a
    // tray icon is set through the shell, and setting it is not free.
    omnuv::TrayIcon m_painted;
    bool m_paintedDark;
    int m_paintedFrame;
    bool m_everPainted;

    // Only alive while the answer is still "not yet": `checkRegistered`
    // stops it on the first verdict, so exactly one line is ever written.
    QTimer* m_registerWatch;
    int m_registerWaitsLeft;
};
