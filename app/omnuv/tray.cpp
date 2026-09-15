#include "tray.h"
#include "autostart.h"
#include "omnuvsession.h"
#include "tunnel.h"
#include "machinemodel.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTimer>
#include <QWindow>

// A minute, asked once a second. Long enough for a shell still assembling the
// notification area during a login, short enough that whatever reads the log
// is not kept waiting on a machine that simply has no tray.
static constexpr int kRegisterPollMs = 1000;
static constexpr int kRegisterWaits = 60;

OmnuvTray* OmnuvTray::create(OmnuvSession* session, QObject* parent)
{
    return new OmnuvTray(session, parent);
}

OmnuvTray::OmnuvTray(OmnuvSession* session, QObject* parent)
    : QObject(parent),
      m_session(session),
      m_icon(new QSystemTrayIcon(this)),
      m_menu(new QMenu()),
      m_state(nullptr),
      m_autostart(nullptr),
      // The session's instance, not a second one: two objects writing the same
      // registry key would disagree about what is true.
      m_auto(session != nullptr ? session->autostart() : nullptr),
      m_registerWatch(new QTimer(this)),
      m_registerWaitsLeft(kRegisterWaits)
{
    // **Our mark, not upstream's.** The tray icon is the one thing on a
    // buyer's screen that is ours all day, so it should say Omnuv rather than
    // Moonlight. Everything that identifies the *program* still names
    // upstream — the window, the executable, the About box — because this is
    // their work with our sign-in added, and the licence and plain decency
    // both require saying so.
    //
    // Falls back rather than showing nothing if the resource is ever missing:
    // an invisible tray icon is a widget that does not exist.
    // `QFile::exists`, not `QIcon::isNull`. A QIcon built from a filename is
    // not "null" merely because nothing has rendered it yet, so testing the
    // icon told us nothing and the fallback fired every time — the tray kept
    // showing upstream's wheel and looked as though the resource were missing.
    // Ask the question that has an answer: is the file in the binary?
    static const QString markPath = QStringLiteral(":/omnuv/omnuv.svg");
    const bool haveMark = QFile::exists(markPath);
    qInfo() << "Omnuv tray icon:" << (haveMark ? markPath : QStringLiteral(":/res/moonlight.svg"));
    m_icon->setIcon(QIcon(haveMark ? markPath : QStringLiteral(":/res/moonlight.svg")));

    QAction* open = m_menu->addAction(tr("Open Omnuv"));
    connect(open, &QAction::triggered, this, &OmnuvTray::openWindow);

    m_menu->addSeparator();

    m_state = m_menu->addAction(QString());
    m_state->setEnabled(false);

    // **Start with Windows, and it reflects the machine rather than us.**
    // The checkbox is set from what the Run key actually says each time the
    // menu is built, so a person who turned it off in Settings sees it off
    // here. A switch that reports our intention instead of the system's state
    // is a switch that lies.
    //
    // Hidden entirely where there is no implementation, rather than shown
    // greyed: an inert control invites the question "why can I not use this".
    if (m_auto != nullptr && m_auto->supported()) {
        m_autostart = m_menu->addAction(tr("Start with %1").arg(
#if defined(Q_OS_WIN)
            tr("Windows")
#elif defined(Q_OS_DARWIN)
            tr("macOS")
#else
            tr("this computer")
#endif
        ));
        m_autostart->setCheckable(true);
        connect(m_autostart, &QAction::toggled, this, &OmnuvTray::toggleAutostart);
        connect(m_menu, &QMenu::aboutToShow, this, &OmnuvTray::refresh);
    }

    m_menu->addSeparator();

    QAction* about = m_menu->addAction(tr("About Omnuv Connect"));
    connect(about, &QAction::triggered, this, &OmnuvTray::showAbout);

    QAction* quit = m_menu->addAction(tr("Quit Omnuv"));
    // The only thing that actually quits. Closing the window hides it; a
    // widget that cannot be quit from its own tray is one people uninstall.
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    m_icon->setContextMenu(m_menu);
    connect(m_icon, &QSystemTrayIcon::activated, this, &OmnuvTray::activated);

    if (m_session != nullptr) {
        connect(m_session, &OmnuvSession::signedInChanged, this, &OmnuvTray::refresh);
        if (m_session->tunnel() != nullptr) {
            connect(m_session->tunnel(), &OmnuvTunnel::changed, this, &OmnuvTray::refresh);
        }
    }

    // The one notification this program sends. `showMessage` is the native
    // balloon on Windows and a StatusNotifier hint on Linux; where the desktop
    // has no notifications it does nothing, which is the right amount of
    // nothing.
    if (m_session != nullptr && m_session->machines() != nullptr) {
        connect(m_session->machines(), &MachineModel::machineBecameReady,
                this, [this](const QString& name) {
                    m_icon->showMessage(tr("%1 is ready").arg(name),
                                        tr("It finished starting and you can connect to it."),
                                        m_icon->icon());
                });
    }

    refresh();

    // Unconditional. A headless session, a desktop with no StatusNotifier
    // host, a kiosk — all real, and all answered by Qt adding the entry if
    // and when an area appears, rather than by us deciding now. The open
    // question after this call is *when* the shell took it, never *whether*
    // we asked.
    m_icon->show();

    m_registerWatch->setInterval(kRegisterPollMs);
    connect(m_registerWatch, &QTimer::timeout, this, &OmnuvTray::checkRegistered);
    checkRegistered();
}

// The one line anything watching this program reads, and the honest moment to
// write it.
//
// `show()` above is the call that registers with the shell, so nothing before
// it can claim anything: a constructor that ran proves this process built some
// objects, which is true on a machine with no shell at all. But `show()`
// returns void, and the `visible` property is our own flag read back — Qt's
// documentation says setting it *"makes the system tray icon visible"*, not
// that the shell accepted it — so asking the icon whether it worked is asking
// the one object that cannot know. `isSystemTrayAvailable()` is the only
// question in this API whose answer comes from outside the process, so that is
// the question, asked in the same breath as `show()`.
//
// When it says no we wait instead of concluding, because Qt documents that it
// adds the entry itself once an area appears, and a notification area that is
// not up yet is the normal state for a program that starts at login. There is
// no signal for it, so this polls: a minute, then a verdict.
//
// **Exactly one `tray=` line per run, and there is always one.** A log with
// neither line is a client that never reached here — a different fault from a
// tray that could not register, and the two must not look alike. printf-style
// rather than `qInfo() <<` so the token is the entire message, with no QDebug
// quoting or trailing space between it and the newline.
//
// What `tray=ready` does not claim: that anyone can see the icon. Windows 11
// files new icons into the overflow flyout by default, and that is a shell
// preference rather than a failure — this says the icon is registered, which
// is the fact a harness can act on.
void OmnuvTray::checkRegistered()
{
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_registerWatch->stop();
        qInfo("tray=ready");
    }
    else if (m_registerWaitsLeft-- > 0) {
        m_registerWatch->start();
    }
    else {
        // Named rather than bare, so a later cause — a platform with no tray
        // at all, a refusal we learn to detect — arrives as a new reason
        // instead of changing what this one means.
        m_registerWatch->stop();
        qInfo("tray=unavailable:no-notification-area");
    }
}

QString OmnuvTray::stateLine() const
{
    if (m_session == nullptr || !m_session->signedIn()) {
        return tr("Not signed in");
    }

    OmnuvTunnel* tunnel = m_session->tunnel();
    if (tunnel == nullptr || !tunnel->available()) {
        return tr("Network client not installed");
    }
    if (tunnel->connected()) {
        return tr("Network: %1").arg(tunnel->address());
    }
    // Says what is true, including when the answer is that we cannot tell. A
    // tray reading "Connected" because nothing has contradicted it yet is
    // worse than one that admits it does not know.
    return tr("Network: %1").arg(tunnel->state());
}

void OmnuvTray::refresh()
{
    const QString line = stateLine();
    if (m_state != nullptr) {
        m_state->setText(line);
    }
    m_icon->setToolTip(tr("Omnuv — %1").arg(line));

    if (m_autostart != nullptr) {
        // Set without re-entering the toggle handler, which would write the
        // value back on every menu open.
        QSignalBlocker block(m_autostart);
        m_autostart->setChecked(m_auto != nullptr && m_auto->enabled());
    }
}

void OmnuvTray::openWindow()
{
    // The application's own window, found rather than held: this object is
    // created before the QML engine builds it, so a pointer captured at
    // construction would be null.
    const auto windows = QGuiApplication::topLevelWindows();
    for (QWindow* w : windows) {
        if (w->isVisible() || w->type() == Qt::Window) {
            w->show();
            w->raise();
            w->requestActivate();
            return;
        }
    }
}

void OmnuvTray::showAbout()
{
    // **Reachable in one click, and it names upstream.** This program
    // auto-starts, so a person who never deliberately launched it is entitled
    // to find out what it is and where its source lives. That is the GPL's
    // requirement and also simply the decent thing.
    QMessageBox::information(
        nullptr,
        tr("About Omnuv Connect"),
        tr("Omnuv Connect is a fork of Moonlight, the open-source game "
           "streaming client, with sign-in to Omnuv and the marketplace "
           "network added.\n\n"
           "Licensed GPL-3.0.\n"
           "Upstream: github.com/moonlight-stream/moonlight-qt\n"
           "This fork, and what was changed: github.com/rsafehelm/omnuv-client"));
}

void OmnuvTray::toggleAutostart(bool on)
{
    if (m_auto != nullptr) { m_auto->setEnabled(on); }
    // Read it back rather than trusting the write: if the registry refused —
    // policy, permissions, a locked-down machine — the menu should show what
    // is true, not what was attempted.
    refresh();
}

void OmnuvTray::activated(QSystemTrayIcon::ActivationReason reason)
{
    // A person who double-clicks means the same as one who clicked.
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        openWindow();
    }
}
