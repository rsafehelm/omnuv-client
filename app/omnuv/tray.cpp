#include "tray.h"
#include "omnuvsession.h"
#include "tunnel.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QWindow>

OmnuvTray* OmnuvTray::createIfSupported(OmnuvSession* session, QObject* parent)
{
    // Ask rather than assume. A headless session, a Linux desktop with no
    // StatusNotifier host, a locked-down kiosk — all real, and all better
    // reported than papered over with an icon nobody can see.
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return nullptr;
    }
    return new OmnuvTray(session, parent);
}

OmnuvTray::OmnuvTray(OmnuvSession* session, QObject* parent)
    : QObject(parent),
      m_session(session),
      m_icon(new QSystemTrayIcon(this)),
      m_menu(new QMenu()),
      m_state(nullptr)
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

    refresh();
    m_icon->show();
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

void OmnuvTray::activated(QSystemTrayIcon::ActivationReason reason)
{
    // A person who double-clicks means the same as one who clicked.
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        openWindow();
    }
}
