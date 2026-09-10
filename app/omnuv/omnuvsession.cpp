#include "omnuvsession.h"
#include "machinemodel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSysInfo>
#include <QtQml>

// How often the machine list is refreshed while the window is open. A machine
// takes minutes to start, so this is about a person watching one come up, not
// about being current to the second.
static const int kRefreshMs = 15 * 1000;

OmnuvSession::OmnuvSession(QObject* parent)
    : QObject(parent),
      m_machines(new MachineModel(this))
{
    QSettings settings;
    m_coreUrl = settings.value(QStringLiteral("omnuv/coreUrl")).toString();
    if (m_coreUrl.isEmpty()) {
        // Set at build time for a packaged client; typed in by hand otherwise.
        m_coreUrl = QString::fromLocal8Bit(qgetenv("OMNUV_CORE_URL"));
    }

    loadToken();

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

// Registered here rather than in main.cpp, which belongs to upstream. This
// runs before the QML engine is built, which is all registration needs.
static void registerOmnuvTypes()
{
    qmlRegisterSingletonType<OmnuvSession>("Omnuv", 1, 0, "Omnuv",
                                           [](QQmlEngine*, QJSEngine*) -> QObject* {
                                               return new OmnuvSession();
                                           });
    qmlRegisterUncreatableType<MachineModel>("Omnuv", 1, 0, "MachineModel",
                                             QStringLiteral("Machines come from Omnuv.machines"));
}
Q_COREAPP_STARTUP_FUNCTION(registerOmnuvTypes)
