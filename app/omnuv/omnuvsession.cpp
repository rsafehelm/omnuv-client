#include "omnuvsession.h"
#include "machinemodel.h"
#include "tray.h"

#include "backend/computermanager.h"
#include "backend/nvcomputer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDesktopServices>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QSysInfo>
#include <QtQml>

// How often the machine list is refreshed while the window is open. A machine
// takes minutes to start, so this is about a person watching one come up, not
// about being current to the second.
static const int kRefreshMs = 15 * 1000;

OmnuvSession::OmnuvSession(QObject* parent)
    : QObject(parent),
      m_machines(new MachineModel(this)),
      m_tunnel(new OmnuvTunnel(this))
{
    QSettings settings;
    m_coreUrl = settings.value(QStringLiteral("omnuv/coreUrl")).toString();
    if (m_coreUrl.isEmpty()) {
        // Set at build time for a packaged client; typed in by hand otherwise.
        m_coreUrl = QString::fromLocal8Bit(qgetenv("OMNUV_CORE_URL"));
    }

    loadToken();

    // The tunnel asks for a key only when the device has never enrolled.
    connect(m_tunnel, &OmnuvTunnel::needsKey, this, &OmnuvSession::fetchDeviceKey);

    m_pollTimer.setSingleShot(false);
    connect(&m_pollTimer, &QTimer::timeout, this, &OmnuvSession::poll);

    m_refreshTimer.setInterval(kRefreshMs);
    connect(&m_refreshTimer, &QTimer::timeout, this, &OmnuvSession::refresh);

    // No fetch here: the view asks when it opens, and again when a person
    // comes back to it from a stream. Doing both would send two requests every
    // time the application starts.
    if (signedIn()) {
        m_refreshTimer.start();
    }
}

QString OmnuvSession::tokenPath()
{
#ifdef Q_OS_WIN
    return QDir::homePath() + QStringLiteral("/AppData/Roaming/Omnuv/token");
#else
    // Exactly where `omnuv-connect sign-in` writes it, on Linux and macOS
    // alike. Sharing the file is the point: one sign-in, two front ends.
    return QDir::homePath() + QStringLiteral("/.config/omnuv/token");
#endif
}

void OmnuvSession::loadToken()
{
    QFile f(tokenPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    m_token = QString::fromUtf8(f.readAll()).trimmed();
}

void OmnuvSession::saveToken(const QString& token)
{
    const QString path = tokenPath();
    QDir().mkpath(QFileInfo(path).path());

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        setStatus(tr("Signed in, but this device could not remember it."));
        return;
    }
    // Before anything is written, so the token is never briefly world-readable.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(token.toUtf8());
    f.write("\n");
}

void OmnuvSession::setCoreUrl(const QString& url)
{
    QString trimmed = url.trimmed();
    while (trimmed.endsWith(QLatin1Char('/'))) {
        trimmed.chop(1);
    }
    if (trimmed == m_coreUrl) {
        return;
    }

    m_coreUrl = trimmed;
    QSettings().setValue(QStringLiteral("omnuv/coreUrl"), m_coreUrl);
    emit coreUrlChanged();
}

void OmnuvSession::setStatus(const QString& text)
{
    if (m_status == text) {
        return;
    }
    m_status = text;
    emit statusChanged();
}

void OmnuvSession::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void OmnuvSession::clearPending()
{
    m_pollTimer.stop();
    m_deviceCode.clear();
    m_userCode.clear();
    m_verificationUri.clear();
    emit pendingChanged();
}

QNetworkRequest OmnuvSession::request(const QString& path, bool authenticated) const
{
    QNetworkRequest req(QUrl(m_coreUrl + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (authenticated) {
        req.setRawHeader("authorization", QStringLiteral("Bearer %1").arg(m_token).toUtf8());
    }
    return req;
}

void OmnuvSession::signIn()
{
    if (m_coreUrl.isEmpty()) {
        setStatus(tr("Enter the address of your Omnuv deployment first."));
        return;
    }

    clearPending();
    setBusy(true);
    setStatus(tr("Asking for a code…"));

    // Named so a person can recognise this device in the console's list and
    // revoke the right one.
    const QJsonObject body {
        { QStringLiteral("client"),
          QStringLiteral("Omnuv on %1").arg(QSysInfo::prettyProductName()) }
    };

    QNetworkReply* reply = m_net.post(request(QStringLiteral("/v1/auth/device"), false),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        setBusy(false);

        if (reply->error() != QNetworkReply::NoError) {
            setStatus(tr("Could not reach %1.").arg(m_coreUrl));
            return;
        }

        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        m_deviceCode = o[QStringLiteral("device_code")].toString();
        m_userCode = o[QStringLiteral("user_code")].toString();
        m_verificationUri = o[QStringLiteral("verification_uri")].toString();
        if (m_deviceCode.isEmpty()) {
            setStatus(tr("That deployment did not offer a sign-in code."));
            return;
        }

        emit pendingChanged();
        setStatus(tr("Waiting for approval in your browser…"));

        // At the interval the server asked for, not one guessed here.
        m_pollTimer.setInterval(qMax(1, o[QStringLiteral("interval")].toInt(3)) * 1000);
        m_pollTimer.start();
    });
}

void OmnuvSession::cancelSignIn()
{
    clearPending();
    setStatus(QString());
}

// One request per tick: 202 means still waiting, 200 means approved, anything
// else means this attempt is over.
void OmnuvSession::poll()
{
    if (m_deviceCode.isEmpty()) {
        m_pollTimer.stop();
        return;
    }

    const QJsonObject body { { QStringLiteral("device_code"), m_deviceCode } };
    QNetworkReply* reply = m_net.post(request(QStringLiteral("/v1/auth/device/token"), false),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 202) {
            return;
        }
        if (code != 200) {
            clearPending();
            setStatus(tr("That sign-in was refused or expired. Try again."));
            return;
        }

        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString token = o[QStringLiteral("token")].toString();
        if (token.isEmpty()) {
            clearPending();
            setStatus(tr("That code was already used. Try again."));
            return;
        }

        m_token = token;
        saveToken(token);
        clearPending();
        emit signedInChanged();
        setStatus(QString());

        m_refreshTimer.start();
        refresh();
    });
}

void OmnuvSession::signOut()
{
    QFile::remove(tokenPath());
    m_token.clear();
    m_refreshTimer.stop();
    m_machines->clear();
    clearPending();
    emit signedInChanged();
    setStatus(tr("Signed out on this device. Revoke it in the console to take the access back for good."));
}

void OmnuvSession::refresh()
{
    if (!signedIn()) {
        return;
    }

    QNetworkReply* reply = m_net.get(request(QStringLiteral("/v1/instances"), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 401 || code == 403) {
            // Revoked in the console, or expired. Say so and stop pretending
            // to be signed in, rather than retrying a token that is dead.
            QFile::remove(tokenPath());
            m_token.clear();
            m_refreshTimer.stop();
            m_machines->clear();
            emit signedInChanged();
            setStatus(tr("This device's access was taken back. Sign in again."));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(tr("Could not reach %1.").arg(m_coreUrl));
            return;
        }

        const QJsonArray machines = QJsonDocument::fromJson(reply->readAll()).array();
        m_machines->replace(machines);
        setStatus(QString());

        // Logged because a person reporting "it says I have no machines" needs
        // this line to tell an empty account from a request that never arrived.
        qInfo() << "omnuv: signed in," << machines.count() << "machine(s)";
    });
}

// An ordinary machine is reached with ssh, and every desktop starts a terminal
// its own way. Try the ones that exist, take the first that starts, and tell
// the caller if none did.
bool OmnuvSession::openTerminal(const QString& host, const QString& user)
{
    const QString target = user.isEmpty() ? host : (user + QLatin1Char('@') + host);

#if defined(Q_OS_WIN)
    // Windows has had OpenSSH since 2018, and `start` gives it its own window.
    return QProcess::startDetached(QStringLiteral("cmd"),
                                   { QStringLiteral("/c"), QStringLiteral("start"),
                                     QStringLiteral("ssh"), target });
#elif defined(Q_OS_DARWIN)
    // Terminal.app registers itself for ssh:// URLs, so this is the one
    // platform where the system already knows the answer.
    return QDesktopServices::openUrl(QUrl(QStringLiteral("ssh://") + target));
#else
    // Debian's alternatives symlink first, since it is whatever the person
    // actually chose; the rest are the terminals that are usually installed.
    const QVector<QPair<QString, QStringList>> candidates {
        { QStringLiteral("x-terminal-emulator"), { QStringLiteral("-e") } },
        { QStringLiteral("gnome-terminal"),      { QStringLiteral("--") } },
        { QStringLiteral("konsole"),             { QStringLiteral("-e") } },
        { QStringLiteral("xfce4-terminal"),      { QStringLiteral("-x") } },
        { QStringLiteral("kitty"),               {} },
        { QStringLiteral("alacritty"),           { QStringLiteral("-e") } },
        { QStringLiteral("xterm"),               { QStringLiteral("-e") } },
    };

    for (const auto& candidate : candidates) {
        if (QStandardPaths::findExecutable(candidate.first).isEmpty()) {
            continue;
        }
        QStringList args = candidate.second;
        args << QStringLiteral("ssh") << target;
        if (QProcess::startDetached(candidate.first, args)) {
            return true;
        }
    }
    return false;
#endif
}

// The key half of joining. The tunnel says when it needs one; this fetches it
// and hands it back. Core is asked for the project's network, then for a
// device named after this computer, so the entry in a person's device list is
// one they recognise.
void OmnuvSession::fetchDeviceKey()
{
    if (!signedIn()) {
        m_tunnel->giveUp(tr("Sign in first, so this device can be added to your network."));
        return;
    }

    QNetworkReply* reply = m_net.get(request(QStringLiteral("/v1/networks"), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const QJsonArray networks = QJsonDocument::fromJson(reply->readAll()).array();
        if (reply->error() != QNetworkReply::NoError || networks.isEmpty()) {
            m_tunnel->giveUp(tr("Could not find your network."));
            return;
        }

        const QString id = networks.first().toObject()[QStringLiteral("id")].toString();
        const QJsonObject body { { QStringLiteral("name"), QSysInfo::machineHostName() } };

        QNetworkReply* add = m_net.post(
            request(QStringLiteral("/v1/networks/%1/devices").arg(id), true),
            QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(add, &QNetworkReply::finished, this, [this, add]() {
            add->deleteLater();

            const QJsonObject o = QJsonDocument::fromJson(add->readAll()).object();
            const QString key = o[QStringLiteral("setup_key")].toString();
            if (add->error() != QNetworkReply::NoError || key.isEmpty()) {
                m_tunnel->giveUp(tr("This device could not be added to your network."));
                return;
            }

            // The management address is deployment configuration, and the only
            // place the API states it is inside the command it hands a person
            // to paste. Read it from there rather than guessing it from Core's.
            const QString command = o[QStringLiteral("command")].toString();
            const QString url = command.section(QStringLiteral("--management-url "), 1, 1)
                                    .section(QLatin1Char(' '), 0, 0);
            if (url.isEmpty()) {
                m_tunnel->giveUp(tr("Your network did not say where to join."));
                return;
            }

            m_tunnel->enrol(url, key);
        });
    });
}

int OmnuvSession::hostRowFor(QObject* computerManager, const QString& address) const
{
    auto manager = qobject_cast<ComputerManager*>(computerManager);
    if (manager == nullptr || address.isEmpty()) {
        return -1;
    }

    // `addNewHostManually` puts the address through
    // QUrl::fromUserInput("moonlight://" + address) and keeps url.host(), and
    // QUrl lowercases hosts. So compare the way it stored it, not the way we
    // typed it, or a machine named GPU-1.internal is never found again.
    const QString wanted = address.toLower();

    const QVector<NvComputer*> computers = manager->getComputers();
    for (int row = 0; row < computers.count(); row++) {
        NvComputer* computer = computers.at(row);
        if (computer != nullptr && computer->manualAddress.address().toLower() == wanted) {
            return row;
        }
    }
    return -1;
}

// Registered here rather than in main.cpp, which belongs to upstream. This
// runs before the QML engine is built, which is all registration needs.
static void registerOmnuvTypes()
{
    qmlRegisterSingletonType<OmnuvSession>("Omnuv", 1, 0, "Omnuv",
                                           [](QQmlEngine*, QJSEngine*) -> QObject* {
                                               auto* session = new OmnuvSession();
                                               // The tray is created here rather than in main.cpp
                                               // because this is our file and that one is
                                               // upstream's. By the time the QML engine resolves
                                               // this singleton the QApplication exists, which is
                                               // what a QSystemTrayIcon needs; at registration
                                               // time it does not.
                                               OmnuvTray::createIfSupported(session, session);
                                               return session;
                                           });
    qmlRegisterUncreatableType<MachineModel>("Omnuv", 1, 0, "MachineModel",
                                             QStringLiteral("Machines come from Omnuv.machines"));
}
Q_COREAPP_STARTUP_FUNCTION(registerOmnuvTypes)
