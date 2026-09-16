#include "omnuvsession.h"
#include "machinemodel.h"
#include "tray.h"
#include "appearance.h"
#include "autostart.h"
#include "credentials.h"
#include "pairing.h"

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
// **One minute, whether or not anybody is watching.**
//
// The plan called for 15 s with the window open and 60 s with only the tray,
// on the reasoning that a person waiting for a machine to come up wants it to
// feel immediate. The operator chose a single minute instead, and it is the
// better default: four times less load on Core for every resident client in
// the fleet, and that number grows with the business while the number of
// people staring at a window does not.
//
// The visible/hidden distinction is kept in the code because it costs nothing
// and the decision may be revisited — but today both sides are the same.
static const int kRefreshVisibleMs = 60 * 1000;
static const int kRefreshHiddenMs = 60 * 1000;

// How long to keep asking Core for a machine's one-time streaming login before
// telling the person it is still setting itself up.
//
// `waiting` is the honest answer while the machine's recipe is still running:
// the guest writes the login on the last line of its install, the provider's
// agent reads it on its next status report, and Core has it a moment after
// that. So the wait is one agent poll, not one install — a minute is generous
// and, unlike a longer one, it ends while the person is still watching.
//
// Asking again costs nothing. Core spends the credential in the statement that
// returns it, and a `waiting` read has no credential to spend.
static const int kLoginTries = 12;
static const int kLoginIntervalMs = 5 * 1000;

OmnuvSession::OmnuvSession(QObject* parent)
    : QObject(parent),
      m_machines(new MachineModel(this)),
      m_tunnel(new OmnuvTunnel(this)),
      m_autostart(new OmnuvAutostart(this)),
      // Built here rather than left to a QML singleton so that it has read
      // the system's two settings, and written its one line to the log,
      // before anything asks. Nothing in QML has to mention it for that to
      // be true.
      m_appearance(new OmnuvAppearance(this)),
      // Shares the session's network access manager: one connection pool, one
      // proxy policy, and the machine's certificate decision is per-request.
      m_pairing(new OmnuvPairing(&m_net, this))
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

    // Every ending of a pairing attempt arrives on one pair of signals,
    // whether it ended at Core or at the machine. The view has one place to
    // listen and one dialog to open.
    connect(m_pairing, &OmnuvPairing::delivered, this, [this]() {
        setStatus(QString());
        emit pairingSucceeded();
    });
    connect(m_pairing, &OmnuvPairing::failed, this,
            [this](const QString& why, const QString& detail) {
        setStatus(QString());
        emit pairingFailed(why, detail);
    });

    m_pollTimer.setSingleShot(false);
    connect(&m_pollTimer, &QTimer::timeout, this, &OmnuvSession::poll);

    m_refreshTimer.setInterval(kRefreshVisibleMs);
    connect(&m_refreshTimer, &QTimer::timeout, this, &OmnuvSession::refresh);

    // No fetch here: the view asks when it opens, and again when a person
    // comes back to it from a stream. Doing both would send two requests every
    // time the application starts.
    m_projectId = settings.value(QStringLiteral("omnuv/projectId")).toString();

    if (signedIn()) {
        m_refreshTimer.start();
        // **Who this person is, and what they may act on.** Everything the
        // window shows is scoped to a project, so the list has to arrive
        // before the first request that names one. It is one call, and it is
        // the same call the console makes.
        fetchIdentity();
    }
}

// `/v1/me` — the user, their organization, and every project they are a member
// of. The chosen project is kept across launches; a remembered id that is no
// longer theirs falls back to the first, because a selection that names
// nothing is worse than a default.
void OmnuvSession::fetchIdentity()
{
    QNetworkReply* reply = m_net.get(request(QStringLiteral("/v1/me"), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonArray projects = o[QStringLiteral("projects")].toArray();

        m_projectNames.clear();
        m_projectIds.clear();
        for (const QJsonValue& v : projects) {
            const QJsonObject p = v.toObject();
            m_projectIds << p[QStringLiteral("id")].toString();
            m_projectNames << p[QStringLiteral("name")].toString();
        }
        if (!m_projectIds.contains(m_projectId)) {
            m_projectId = m_projectIds.value(0);
            QSettings().setValue(QStringLiteral("omnuv/projectId"), m_projectId);
        }
        emit projectsChanged();
        refresh();
    });
}

void OmnuvSession::selectProject(const QString& id)
{
    if (id == m_projectId || !m_projectIds.contains(id)) {
        return;
    }
    m_projectId = id;
    QSettings().setValue(QStringLiteral("omnuv/projectId"), m_projectId);
    emit projectsChanged();
    // The machines on screen belong to the project that was chosen a moment
    // ago; leaving them there would be showing one project's estate under
    // another's name.
    m_machines->clear();
    refresh();
}

// The query every project-scoped call carries. Empty until the identity has
// arrived, which Core reads as "my default project" — the same thing it did
// before this existed, so a first paint is never worse than it was.
QString OmnuvSession::projectQuery() const
{
    return m_projectId.isEmpty() ? QString()
                                 : QStringLiteral("?project=%1").arg(m_projectId);
}


void OmnuvSession::loadToken()
{
    m_token = OmnuvCredentials::load();
}

void OmnuvSession::saveToken(const QString& token)
{
    // Says so when it could not. A client that signs in, fails to remember it,
    // and says nothing sends the person through the whole browser dance again
    // at the next launch with no idea why.
    if (!OmnuvCredentials::store(token)) {
        setStatus(tr("Signed in, but this device could not remember it."));
    }
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
            // **Named, because "refused or expired" is three different things
            // and one of them is "we never asked properly".** A poll that dies
            // on a transport error has an HTTP status of 0, which is not a
            // refusal by anybody — and telling those apart from the outside
            // cost a rig cycle and a proxy log on 16 September.
            qWarning("omnuv: sign-in poll: http=%d error=%d %s", code, int(reply->error()),
                     qPrintable(reply->errorString()));
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

        // **A device that holds a token remembers the deployment it holds it
        // for.** The address is otherwise persisted only by `setCoreUrl()` —
        // which is a person typing it into the window — so a device signed in
        // from a shell, where the address came from `OMNUV_CORE_URL`, forgot
        // it the moment that process ended. The next run built its requests
        // against an empty base, and `QNetworkAccessManager` failed them
        // locally: nothing reached the network, and the client reported
        // "Could not find your network" for an address it no longer had.
        // Found on the rig on 16 September, by an access log that showed the
        // request had never been made.
        QSettings().setValue(QStringLiteral("omnuv/coreUrl"), m_coreUrl);
        clearPending();
        emit signedInChanged();
        setStatus(QString());

        m_refreshTimer.start();
        fetchIdentity();
    });
}

void OmnuvSession::signOut()
{
    // Both the credential store and any file an older version left behind.
    // Signing out must not leave a token anywhere.
    OmnuvCredentials::clear();
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

    QNetworkReply* reply = m_net.get(request(QStringLiteral("/v1/instances") + projectQuery(), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 401 || code == 403) {
            // Revoked in the console, or expired. Say so and stop pretending
            // to be signed in, rather than retrying a token that is dead.
            OmnuvCredentials::clear();
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

    QNetworkReply* reply = m_net.get(request(QStringLiteral("/v1/networks") + projectQuery(), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const QJsonArray networks = QJsonDocument::fromJson(reply->readAll()).array();
        if (reply->error() != QNetworkReply::NoError || networks.isEmpty()) {
            m_tunnel->giveUp(tr("Could not find your network."));
            return;
        }

        // **The project's network, and a project has exactly one.** Since
        // 16 September that is a database invariant rather than a convention:
        // a project is created with its network in one transaction (0128's
        // sibling change) and a trigger refuses to delete one while the
        // project is alive (0129). `projectQuery()` above scopes the list to
        // the project this window is showing, so `first` is that project's
        // network and not whichever network the account happened to list
        // first — which is what a device joining the *wrong* tenant would
        // look like.
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

// **A pairing nobody types.**
//
// Moonlight's PIN has always been a number a person reads off one screen and
// types into another, because the client had no way to prove to the machine
// that it was entitled to pair. A machine the marketplace deployed does: its
// recipe mints a single-use login for its own streaming host, writes it where
// only root can read it, and the provider's agent carries it up to Core on the
// ordinary status report. The owner collects it once, here, and spends it on
// the machine — so the number still exists and nobody ever sees it.
//
// Three things this function must get right, and each is a decision rather
// than a detail:
//
// **The order.** ComputerModel::pairComputer() has already been called by the
// time this runs, and that is not an accident of sequencing — the identifier a
// PIN is addressed to does not exist until the client's own pairing request is
// waiting on the machine. See `pairing.cpp`.
//
// **The credential is spent on read.** Core nulls the password in the same
// statement that returns it, so there is exactly one chance per deployment.
// Everything that can fail cheaply is therefore done first: the deployment is
// found before the login is asked for, and the login is asked for only when
// there is a pairing in flight to spend it on.
//
// **`waiting` is not a failure.** It is the normal answer while the machine is
// still installing, and drawing it as an error would send a person to fix a
// machine that is working.
void OmnuvSession::deliverPin(int row, const QString& pin)
{
    const QString name = m_machines->nameAt(row);
    const QString instanceId = m_machines->idAt(row);

    if (!signedIn() || instanceId.isEmpty() || m_machines->hostAt(row).isEmpty()) {
        m_pairing->giveUp(tr("This device is not signed in to Omnuv, so it cannot collect the "
                             "login %1 published for itself.").arg(name));
        return;
    }

    setStatus(tr("Pairing with %1…").arg(name));

    // No `?project=` — deliberately the same omission as refresh(), so the
    // deployments and the machines come from the same project Core picks by
    // default. A deployment in another project belongs to a machine that is
    // not in the list this row came from.
    QNetworkReply* reply = m_net.get(request(QStringLiteral("/v1/deployments") + projectQuery(), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply, row, pin, name, instanceId]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            m_pairing->giveUp(tr("Omnuv could not be reached to collect %1's login.").arg(name),
                              reply->errorString());
            return;
        }

        // The buyer API's machine list carries no deployment, and its
        // deployment list carries the machine — so the join is on this side,
        // on the identifier both of them do carry.
        QString deploymentId;
        const QJsonArray deployments = QJsonDocument::fromJson(reply->readAll()).array();
        for (const QJsonValue& value : deployments) {
            const QJsonObject d = value.toObject();
            if (d[QStringLiteral("instance_id")].toString() == instanceId) {
                deploymentId = d[QStringLiteral("id")].toString();
                break;
            }
        }

        if (deploymentId.isEmpty()) {
            m_pairing->giveUp(tr("%1 was not set up from an Omnuv recipe, so it has no login to "
                                 "hand over. Pair it by hand this once.").arg(name),
                              tr("No deployment lists this machine."));
            return;
        }

        collectStreamLogin(row, deploymentId, pin, kLoginTries);
    });
}

void OmnuvSession::collectStreamLogin(int row, const QString& deploymentId,
                                      const QString& pin, int triesLeft)
{
    const QString name = m_machines->nameAt(row);
    const QString host = m_machines->hostAt(row);

    QNetworkReply* reply = m_net.get(
        request(QStringLiteral("/v1/deployments/%1/stream-credentials").arg(deploymentId), true));

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, row, deploymentId, pin, triesLeft, name, host]() {
        reply->deleteLater();

        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code != 200) {
            m_pairing->giveUp(
                code == 404
                    ? tr("Omnuv no longer has a record of %1's setup, so there is no login to "
                         "collect.").arg(name)
                    : tr("Omnuv would not hand over %1's login.").arg(name),
                tr("GET stream-credentials returned %1.")
                    .arg(code == 0 ? reply->errorString() : QString::number(code)));
            return;
        }

        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString status = o[QStringLiteral("status")].toString();
        const QString user = o[QStringLiteral("user")].toString();

        // Branch on Core's word, never on the sentence around it. These three
        // are the whole vocabulary of that endpoint.
        if (status == QStringLiteral("waiting")) {
            // The machine has not published its login yet, which is what a
            // machine still installing looks like. Ask again; say so meanwhile.
            if (triesLeft > 1) {
                setStatus(tr("%1 is still setting itself up…").arg(name));
                QTimer::singleShot(kLoginIntervalMs, this,
                                   [this, row, deploymentId, pin, triesLeft]() {
                    collectStreamLogin(row, deploymentId, pin, triesLeft - 1);
                });
                return;
            }
            m_pairing->giveUp(tr("%1 has not finished setting itself up, so it has not published "
                                 "the login this device needs. Try again in a few minutes, or "
                                 "pair by hand.").arg(name),
                              tr("stream-credentials answered waiting %1 times.").arg(kLoginTries));
            return;
        }

        if (status == QStringLiteral("delivered")) {
            // Collected once already, by this device or another, and Core
            // cannot reissue it: the password was cleared in the statement
            // that returned it. Say what is actually left to do.
            m_pairing->giveUp(
                user.isEmpty()
                    ? tr("%1's one-time login has already been collected, and it cannot be "
                         "issued again. Pair from the device that collected it, or deploy the "
                         "machine again to get a fresh one.").arg(name)
                    : tr("%1's one-time login has already been collected, and it cannot be "
                         "issued again. Pair from the device that collected it, or deploy the "
                         "machine again. Its login name is %2.").arg(name, user),
                tr("stream-credentials answered delivered. The machine still holds its own copy "
                   "in /etc/onv/recipe-stream-credential."));
            return;
        }

        // `ready`. The password exists in this reply and nowhere else, now and
        // for good: it is not written to settings, not put in the credential
        // store, not logged, and not held on any object. It goes from here
        // into the one request that spends it.
        const QString password = o[QStringLiteral("password")].toString();
        if (status != QStringLiteral("ready") || user.isEmpty() || password.isEmpty()) {
            m_pairing->giveUp(tr("Omnuv answered about %1's login in a way this device did not "
                                 "understand.").arg(name),
                              tr("stream-credentials answered \"%1\".").arg(status));
            return;
        }

        setStatus(tr("Pairing with %1…").arg(name));
        m_pairing->deliver(host, pin,
                           // The name the machine will list this device under,
                           // and the same one it was enrolled on the network
                           // with, so one device reads as one device.
                           QSysInfo::machineHostName(),
                           // Which of the requests waiting on that machine is
                           // ours: the address it saw us arrive from.
                           m_tunnel->address(),
                           user, password);
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

bool OmnuvSession::shouldStartHidden()
{
    QSettings settings;
    const bool ranBefore = settings.value(QStringLiteral("omnuv/hasRunBefore"), false).toBool();
    settings.setValue(QStringLiteral("omnuv/hasRunBefore"), true);
    settings.sync();

    return ranBefore && m_autostart != nullptr && m_autostart->enabled();
}

void OmnuvSession::setWindowVisible(bool visible)
{
    if (visible) {
        // Mica behind the window, asked for from here because this is the first
        // moment there is a window to ask about: applying it once after
        // `engine.load()` would also work until the first time Qt destroyed and
        // recreated the native window, and then stop, silently. Idempotent per
        // handle, so the repetition costs a comparison — see
        // `OmnuvAppearance::applyBackdrop`.
        //
        // Above the early return below rather than after it, because that
        // return fires on the very first call: the refresh timer already starts
        // at the visible interval.
        m_appearance->applyBackdrop();
    }

    const int want = visible ? kRefreshVisibleMs : kRefreshHiddenMs;
    if (m_refreshTimer.interval() == want) {
        return;
    }
    m_refreshTimer.setInterval(want);
    // Logged because it is otherwise invisible: the rate only shows up as the
    // gap between two requests, and on a client that is not signed in there
    // are no requests to measure.
    qInfo("Omnuv: window %s, polling every %d s",
          visible ? "shown" : "hidden", want / 1000);

    // Refresh straight away when the window opens rather than making the
    // person wait out the remainder of a 60-second tick to see current state.
    // The opposite direction needs nothing: going quiet can wait.
    if (visible && m_refreshTimer.isActive()) {
        refresh();
    }
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
                                               OmnuvTray::create(session, session);
                                               return session;
                                           });
    qmlRegisterUncreatableType<MachineModel>("Omnuv", 1, 0, "MachineModel",
                                             QStringLiteral("Machines come from Omnuv.machines"));

    // Two QML files of ours, registered as types in the same module so that the
    // `import Omnuv 1.0` most files already carry brings them along.
    //
    // This is how a QML file becomes a type in a **qmake** project. There is no
    // `qmldir` anywhere in this tree and no QML module: `qt_add_qml_module` is
    // a CMake command, and the alternative — a directory import — would need an
    // extra import line in every consumer including upstream's `main.qml`. A
    // URL registration needs none, because the module is already imported.
    qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/omnuv/Theme.qml")),
                             "Omnuv", 1, 0, "Theme");

    // And the view itself, so `main.qml` can ask whether what the StackView is
    // showing is ours — `stackView.currentItem instanceof OmnuvView` — and keep
    // upstream's toolbar off our screens without hard-coding a URL or a name in
    // a string comparison.
    //
    // The view is *pushed* by URL (`main.qml`, `push("qrc:/omnuv/OmnuvView.qml")`)
    // rather than by this type, and that is fine: a composite type is identified
    // by its URL, so the instance the push creates is an instance of this type.
    // Upstream relies on exactly that today — `PcView` is pushed as the string
    // `qrc:/gui/PcView.qml` and matched with `instanceof PcView` — the only
    // difference being that upstream's name comes from QML's implicit
    // same-directory resolution, which does not reach us in `qrc:/omnuv/`.
    qmlRegisterType(QUrl(QStringLiteral("qrc:/omnuv/OmnuvView.qml")),
                    "Omnuv", 1, 0, "OmnuvView");

    // Nothing else of ours is registered, and nothing needs to be. A `.qml`
    // in `qrc:/omnuv/` is a type to every other file in that directory through
    // QML's implicit directory import, exactly as upstream's `PcView` finds
    // `NavigableDialog` in `qrc:/gui/`. Two registrations were added here on
    // 15 September on the belief that implicit resolution did not reach a
    // `qrc:` directory; the window stayed blank, and the cause was a property
    // that does not exist (`font.families`, see `Theme.qml`). They were
    // withdrawn with the belief.
}
Q_COREAPP_STARTUP_FUNCTION(registerOmnuvTypes)
