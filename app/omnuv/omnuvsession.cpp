#include "omnuvsession.h"
#include "machinemodel.h"
#include "tray.h"
#include "appearance.h"
#include "autostart.h"
#include "credentials.h"
#include "pairing.h"
#include <QUuid>

#include "backend/computermanager.h"
#include "backend/nvcomputer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDesktopServices>
#include <QProcess>
#include <QGuiApplication>
#include <QSettings>
#include <QRegularExpression>
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

namespace {
bool nonemptyString(const QJsonObject& object, const char* key)
{
    return object.value(QLatin1String(key)).isString()
        && !object.value(QLatin1String(key)).toString().trimmed().isEmpty();
}

// Validate the identity fields used for scoping before changing any state.
// Unknown fields and future status/role values remain compatible.
bool validProjects(const QJsonValue& projects)
{
    if (!projects.isArray()) return false;
    QSet<QString> ids;
    for (const auto& value : projects.toArray()) {
        if (!value.isObject()) return false;
        const auto project = value.toObject();
        if (!nonemptyString(project, "id") || !nonemptyString(project, "name")
            || ids.contains(project["id"].toString())) return false;
        ids.insert(project["id"].toString());
    }
    return true;
}

bool validMachines(const QJsonDocument& doc)
{
    if (!doc.isArray()) return false;
    QSet<QString> ids;
    for (const auto& value : doc.array()) {
        if (!value.isObject()) return false;
        const auto machine = value.toObject();
        if (!nonemptyString(machine, "id") || !nonemptyString(machine, "name")
            || !nonemptyString(machine, "status") || ids.contains(machine["id"].toString())) return false;
        ids.insert(machine["id"].toString());
        // These fields can be absent or null while a machine is being placed.
        for (const auto key : {"private_name", "private_ip", "stream_app", "default_user", "last_error"}) {
            const auto field = machine.value(QLatin1String(key));
            if (!field.isUndefined() && !field.isNull() && !field.isString()) return false;
        }
    }
    return true;
}
}

namespace {
QString& coreUrlOverride()
{
    static QString url;
    return url;
}
}

void OmnuvSession::setCoreUrlOverride(const QString& url)
{
    QString trimmed = url.trimmed();
    while (trimmed.endsWith(QLatin1Char('/'))) trimmed.chop(1);
    coreUrlOverride() = trimmed;
}

OmnuvSession::OmnuvSession(QObject* parent)
    : QObject(parent),
      // The slot of the Core this session talks to, and no other: see
      // credentials.h.
      m_storeToken([this](const QString& token) { return m_fixture || OmnuvCredentials::store(tokenOrigin(), token); }),
      m_clearToken([this]() {
          return m_fixture || OmnuvCredentials::clear(tokenOrigin(), OmnuvCredentials::origin(
              QSettings().value(QStringLiteral("omnuv/coreUrl")).toString()));
      }),
      m_machines(new MachineModel(this)),
      m_estate(new OmnuvEstate([this](const QString& path) { return m_net.get(request(path, true)); }, this)),
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
    // Honour a Core's Strict-Transport-Security, as the update checker does.
    m_net.setStrictTransportSecurityEnabled(true);
    QSettings settings;
    if (m_fixture) {
        // **A guest in somebody else's house.** `OMNUV_FIXTURE_URL` is how the
        // two loops photograph a populated window against
        // `deployment/client-linux/fixture-core.py` — and the Windows rig is
        // signed in to production, with a real token in Credential Manager and
        // a saved address the end-to-end harness depends on. So a fixture run
        // reads neither and writes neither: the address is the variable, the
        // token is the one string only the fixture accepts, and every place
        // below that would remember or forget something asks `m_fixture`
        // first. A real token is never sent to a fixture, because it is never
        // loaded.
        m_coreUrl = qEnvironmentVariable("OMNUV_FIXTURE_URL");
        m_token = QStringLiteral("fixture-token");
        qInfo() << "omnuv: fixture session against" << m_coreUrl << "- saved account and startup state are bypassed";
    } else {
        if (!coreUrlOverride().isEmpty()) {
            // Named for this run: beats the saved address and is never saved.
            m_coreUrl = coreUrlOverride();
            m_coreUrlOverridden = true;
        } else {
            m_coreUrl = settings.value(QStringLiteral("omnuv/coreUrl")).toString();
        }
        if (m_coreUrl.isEmpty()) {
            // Set at build time for a packaged client; typed in by hand otherwise.
            m_coreUrl = QString::fromLocal8Bit(qgetenv("OMNUV_CORE_URL"));
        }
        // H2: whichever of the three it came from, an address the token cannot
        // safely go to is not used, so no token is loaded for it either. One
        // saved by an earlier version is where this bites.
        if (!m_coreUrl.isEmpty() && !OmnuvCredentials::secureCore(m_coreUrl)) {
            qWarning().noquote() << "omnuv: refusing the Core address" << m_coreUrl << "- not https";
            m_insecureCoreUrl = m_coreUrl;
            m_coreUrl.clear();
            m_coreUrlOverridden = false;
            // Held, and read by the window when it binds: the one place a
            // person sees why the address they saved is gone.
            m_status = tr("%1 is not an https:// address, so Omnuv will not sign in there. Enter its https:// address.").arg(m_insecureCoreUrl);
        }

        loadToken();
    }

    // The tunnel asks for a key only when the device has never enrolled.
    connect(m_tunnel, &OmnuvTunnel::needsKey, this, &OmnuvSession::fetchDeviceKey);
    connect(m_tunnel, &OmnuvTunnel::changed, this, &OmnuvSession::observeEnrollment);
    connect(m_tunnel, &OmnuvTunnel::observationExpired, this, [this]() { ++m_enrollmentRequest; });

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
    connect(&m_refreshTimer, &QTimer::timeout, this, [this]() { refresh(); });

    // No fetch here: the view asks when it opens, and again when a person
    // comes back to it from a stream. Doing both would send two requests every
    // time the application starts.
    if (!m_fixture) {
        // This Core's own choice; the single key every Core once shared is
        // read only when there is none, and is never written again (H13).
        m_projectId = settings.value(projectSettingKey(m_coreUrl),
                                     settings.value(QStringLiteral("omnuv/projectId"))).toString();
    }

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
    // One at a time: the constructor and the window's first refresh both ask.
    if (m_identityPending) {
        return;
    }
    m_identityPending = true;
    const auto context = m_context;
    const auto sequence = ++m_identityRequest;
    QNetworkReply* reply = m_net.get(request(QStringLiteral("/v1/me"), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context, sequence]() {
        reply->deleteLater();
        if (context != m_context || sequence != m_identityRequest) return;
        m_identityPending = false;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // **Only a 401 means the token is gone (H22).** A 403 is Core refusing
        // this request — a policy, a fenced project — with the token still
        // good, and it signed the person out. It is shown below, in Core's
        // words, like any other refusal.
        if (code == 401) {
            accessTakenBack();
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            // Asked again on the next refresh; meanwhile, said.
            const QString said = QJsonDocument::fromJson(reply->readAll()).object()
                                     .value(QStringLiteral("error")).toString().trimmed();
            setReadProblem(true, code == 0 ? tr("Could not reach %1.").arg(m_coreUrl)
                      : !said.isEmpty() ? tr("Omnuv could not say who this is: %1").arg(said)
                      : tr("Omnuv could not say who this is just now."));
            return;
        }
        QJsonParseError parsed;
        const auto doc = QJsonDocument::fromJson(reply->readAll(), &parsed);
        const auto o = doc.object();
        if (parsed.error != QJsonParseError::NoError || !doc.isObject()
            || !nonemptyString(o, "email") || !nonemptyString(o, "organization_name")
            || !nonemptyString(o, "organization_role") || !validProjects(o["projects"])) {
            setReadProblem(true, tr("Omnuv returned account details this version cannot read. Your previous account view has been kept."));
            return;
        }
        setReadProblem(true, QString());
        const QJsonArray projects = o[QStringLiteral("projects")].toArray();
        if (m_identityKnown && !m_accountId.isEmpty() && m_accountId != o.value("user_id").toString()) {
            finishPairing(); ++m_context; ++m_machineRequest;
            m_machines->clear();
            if (m_tunnel->busy()) m_tunnel->giveUp(tr("Network enrollment was cancelled because the account changed."));
            emit connectionContextChanged();
        }
        m_accountEmail = o[QStringLiteral("email")].toString();
        m_accountId = o[QStringLiteral("user_id")].toString();
        considerCanonicalCore(o.value(QStringLiteral("core")).toString());
        m_identityKnown = true;
        m_estate->setIdentity(o[QStringLiteral("organization_name")].toString(),
                              o[QStringLiteral("organization_role")].toString());

        m_projectNames.clear();
        m_projectIds.clear();
        for (const QJsonValue& v : projects) {
            const QJsonObject p = v.toObject();
            m_projectIds << p[QStringLiteral("id")].toString();
            m_projectNames << p[QStringLiteral("name")].toString();
        }
        if (!m_projectIds.contains(m_projectId)) {
            finishPairing();
            ++m_context;
            ++m_machineRequest;
            m_machines->clear();
            setReadProblem(false, QString());
            m_tunnel->clearOperationError();
            if (m_tunnel->busy()) m_tunnel->giveUp(tr("Network enrollment was cancelled because the account or project changed."));
            m_projectId = m_projectIds.value(0);
            emit connectionContextChanged();
            rememberProject();
        }
        updateNetworkScope();
        m_estate->setProject(m_projectId);
        if (m_projectIds.isEmpty()) {
            // Said once per identity read, for the person asking why the
            // window is empty; the loop's grading reads it too.
            qInfo() << "omnuv: signed in, no project";
            m_machines->clear();
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
    finishPairing();
    ++m_context;
    ++m_machineRequest;
    ++m_identityRequest;
    m_identityPending = false;
    m_tunnel->clearOperationError();
    if (m_tunnel->busy()) m_tunnel->giveUp(tr("Network enrollment was cancelled because the account or project changed."));
    m_projectId = id;
    updateNetworkScope();
    setReadProblem(false, QString());
    emit connectionContextChanged();
    rememberProject();
    emit projectsChanged();
    // The machines on screen belong to the project that was chosen a moment
    // ago; leaving them there would be showing one project's estate under
    // another's name.
    m_machines->clear();
    m_estate->setProject(m_projectId);
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


QString OmnuvSession::projectSettingKey(const QString& coreUrl)
{
    return QStringLiteral("omnuv/projectId/") + OmnuvCredentials::origin(coreUrl);
}

// Not for an address named for this run only: a harness run against the test
// Core must not rewrite the project a person chose on production (H13).
void OmnuvSession::rememberProject()
{
    if (m_coreUrlOverridden || tokenOrigin().isEmpty()) return;
    remember(projectSettingKey(m_coreUrl), m_projectId);
}

void OmnuvSession::remember(const QString& key, const QString& value)
{
    if (!m_fixture) {
        QSettings().setValue(key, value);
    }
}

void OmnuvSession::loadToken()
{
    // A fixture session reads no real credential, ever: see the constructor.
    if (m_fixture) return;
    // The old single-slot token belonged to the address saved beside it, so
    // that is the only origin it may move into.
    const QString savedOrigin = OmnuvCredentials::origin(
        QSettings().value(QStringLiteral("omnuv/coreUrl")).toString());
    m_token = OmnuvCredentials::load(tokenOrigin(), savedOrigin);
}

QString OmnuvSession::tokenOrigin() const
{
    return OmnuvCredentials::origin(m_coreUrl);
}

void OmnuvSession::setCredentialWarning(const QString& warning)
{
    if (m_credentialWarning == warning) return;
    m_credentialWarning = warning;
    if (!warning.isEmpty()) qWarning().noquote() << "omnuv:" << warning;
    emit credentialWarningChanged();
}

void OmnuvSession::saveToken(const QString& token)
{
    setCredentialWarning(m_storeToken(token) ? QString()
        : tr("Signed in for this session, but this device could not save your login. You may need to sign in again after closing the app."));
}

void OmnuvSession::setReadProblem(bool identity, const QString& problem)
{
    auto& previous = identity ? m_identityProblem : m_machineProblem;
    if (previous == problem) return;
    previous = problem;
    if (!problem.isEmpty()) qWarning().noquote() << "omnuv:" << problem;
    emit readProblemChanged();
}

void OmnuvSession::retryReads()
{
    if (!signedIn()) return;
    fetchIdentity();
    refresh(true);
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
    // **Not while a join is on its way (H14, 23 September 2026).** Once the
    // daemon has accepted `enrol-v1` it carries on whatever the window does,
    // so a switch then left this device joining the old Core's network with
    // nobody watching, and the new Core would find it on "another network".
    // A join resolves or times out within the tunnel's deadline, so this
    // waits at most that long. Core-side work — an attempt being created or
    // cleaned up — is scoped to its deployment and may be left behind.
    if (m_tunnel->joinInFlight()) {
        setStatus(tr("This device is joining a network at %1. Switch when it has finished.").arg(m_coreUrl));
        return;
    }
    // H2: a Core the token cannot safely be sent to is refused, not saved.
    if (!OmnuvCredentials::secureCore(trimmed)) {
        setStatus(tr("Use an https:// address. Omnuv will not send your sign-in over plain http."));
        return;
    }

    // **Switching, not signing out (22 September 2026).** This used to call
    // `signOut()`, which deleted the only token there was: moving to the test
    // Core and back cost production's sign-in. Each Core has its own slot now,
    // so the context is dropped, the address changes, and that Core's own
    // sign-in (if there is one) is read. The other Core's is left alone.
    invalidateContext();
    clearPending();
    m_refreshTimer.stop();
    m_token.clear();
    m_identityKnown = false;
    m_coreUrl = trimmed;
    m_coreUrlOverridden = false;
    remember(QStringLiteral("omnuv/coreUrl"), m_coreUrl);
    // That Core's own last choice, checked against its /v1/me as any is.
    if (!m_fixture) m_projectId = QSettings().value(projectSettingKey(m_coreUrl)).toString();
    loadToken();
    emit coreUrlChanged();
    emit signedInChanged();
    setStatus(QString());
    if (signedIn()) {
        m_refreshTimer.start();
        fetchIdentity();
    }
}

void OmnuvSession::setStatus(const QString& text)
{
    if (m_status == text) {
        return;
    }
    // Logged, because it is the one sentence a person reads about what went
    // wrong, and "what did it say" is the first question afterwards.
    if (!text.isEmpty()) {
        qInfo().noquote() << "omnuv: status:" << text;
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
    ++m_authAttempt;
    m_pollPending = false;
    setBusy(false);
    m_pollTimer.stop();
    m_deviceCode.clear();
    m_userCode.clear();
    m_verificationUri.clear();
    emit pendingChanged();
}

QNetworkRequest OmnuvSession::request(const QString& path, bool authenticated) const
{
    return requestAt(m_coreUrl, path, authenticated);
}

QNetworkRequest OmnuvSession::requestAt(const QString& base, const QString& path, bool authenticated) const
{
    QNetworkRequest req(QUrl(base + path));
    req.setTransferTimeout(15000);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // H2b: a redirect is an answer, not an instruction. Qt follows https to
    // https by default and may carry the Authorization header to whatever host
    // the redirect names; Core never redirects an API call, so a 3xx arrives
    // as the error it is.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    // The last line of H2: whatever set the address, a token never goes out
    // in the clear. The fixture's token is not a credential.
    if (authenticated && (m_fixture || OmnuvCredentials::secureCore(base))) {
        req.setRawHeader("authorization", QStringLiteral("Bearer %1").arg(m_token).toUtf8());
    }
    return req;
}

void OmnuvSession::signIn()
{
    if (m_coreUrl.isEmpty() && !m_insecureCoreUrl.isEmpty()) {
        setStatus(tr("%1 is not an https:// address, so Omnuv will not sign in there. Enter its https:// address.").arg(m_insecureCoreUrl));
        return;
    }
    if (m_coreUrl.isEmpty()) {
        setStatus(tr("Enter the address of your Omnuv deployment first."));
        return;
    }

    clearPending();
    const auto attempt = m_authAttempt;
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, attempt]() {
        reply->deleteLater();
        if (attempt != m_authAttempt) return;
        setBusy(false);

        if (reply->error() != QNetworkReply::NoError) {
            // **What went wrong, in the network library's words (H21).** A
            // name that does not resolve, a certificate that does not verify
            // and a refused connection were one sentence, and each has a
            // different fix.
            setStatus(tr("Could not reach %1: %2").arg(m_coreUrl, reply->errorString()));
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

        // At the interval the server asked for, not one guessed here, and for
        // as long as the code lives.
        m_codeDeadline.setRemainingTime(qint64(qMax(1, o[QStringLiteral("expires_in")].toInt(600))) * 1000);
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

    if (m_pollPending) return;
    m_pollPending = true;
    const auto attempt = m_authAttempt;
    const QJsonObject body { { QStringLiteral("device_code"), m_deviceCode } };
    QNetworkReply* reply = m_net.post(request(QStringLiteral("/v1/auth/device/token"), false),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, attempt]() {
        reply->deleteLater();
        if (attempt != m_authAttempt) return;
        m_pollPending = false;

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
            // **A failure in passing is not an answer (H20).** No answer, or a
            // 5xx, ended the sign-in — after the person may already have
            // approved it, with the console telling them the application can
            // finish now. It is polled again until the code expires; only
            // Core saying no (a 4xx) ends it.
            if ((code == 0 || code >= 500) && !m_codeDeadline.hasExpired()) {
                setStatus(tr("Omnuv is not answering right now; still waiting for your approval…"));
                return;
            }
            // **Core's own reason (H23).** Refused, expired and already used
            // are three answers, and its 400 says which.
            const auto said = QJsonDocument::fromJson(reply->readAll()).object()
                                  .value(QStringLiteral("error")).toString().trimmed();
            clearPending();
            if (code == 0 || code >= 500)
                setStatus(tr("The code expired while Omnuv was not answering. Try again."));
            else if (!said.isEmpty())
                setStatus(tr("That sign-in did not complete: %1. Try again.").arg(said));
            else
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

        invalidateContext();
        m_token = token;
        // **A device that holds a token remembers the deployment it holds it
        // for.** The address is otherwise persisted only by `setCoreUrl()` —
        // which is a person typing it into the window — so a device signed in
        // from a shell, where the address came from `OMNUV_CORE_URL`, forgot
        // it the moment that process ended. The next run built its requests
        // against an empty base, and `QNetworkAccessManager` failed them
        // locally: nothing reached the network, and the client reported
        // "Could not find your network" for an address it no longer had.
        // Found on the rig on 16 September, by an access log that showed the
        // request had never been made. Not for an address named for this run
        // only: see setCoreUrlOverride.
        //
        // **The address first, and on disk, then the token (H15).** In the
        // other order a crash between the two left the new Core's token in
        // its slot and the old Core's address saved, and the next launch
        // opened the old Core. This way round the worst case is the new
        // address with no token yet, which asks for a sign-in: what it is.
        if (!m_coreUrlOverridden && !m_fixture) {
            remember(QStringLiteral("omnuv/coreUrl"), m_coreUrl);
            QSettings().sync();
        }
        saveToken(token);

        clearPending();
        emit signedInChanged();
        setStatus(QString());

        m_refreshTimer.start();
        fetchIdentity();
    });
}

// **One Core, one address (H12, 23 September 2026).** The same Core answers
// as api.omnuv.com, console.omnuv.com and the bare domain, and this app keeps a
// sign-in and a network membership per address — so a person who typed the
// console's address had a second sign-in, and a device enrolled under one
// spelling was on "another network" under the next, and was asked to move.
//
// Core names its own address in /v1/me. The session moves there only when it
// is proved to be the same place: this token answers there, for this account.
// A name that does not answer, or answers for someone else, changes nothing —
// so a malicious server can at most point the app at a Core where its own
// token is worthless. Never under an address named for one run, never during
// a join, and never away from a membership made under the current address:
// that device stays where it is rather than being asked to move.
void OmnuvSession::considerCanonicalCore(const QString& named)
{
    if (named.isEmpty() || m_coreUrlOverridden || m_canonicalProbe) return;
    const QString canonical = OmnuvCredentials::origin(named);
    if (canonical.isEmpty() || canonical == tokenOrigin() || !OmnuvCredentials::secureCore(canonical)) return;
    if (m_tunnel->joinInFlight()) return;
    const auto member = m_tunnel->membership();
    if (!member.isEmpty() && member.value(QStringLiteral("core_url")).toString() != canonical) return;
    m_canonicalProbe = true;
    const auto context = m_context;
    const auto account = m_accountId;
    QNetworkReply* reply = m_net.get(requestAt(canonical, QStringLiteral("/v1/me"), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context, account, canonical]() {
        reply->deleteLater();
        m_canonicalProbe = false;
        if (context != m_context || account != m_accountId || m_tunnel->joinInFlight()) return;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto there = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("user_id")).toString();
        if (code != 200 || there.isEmpty() || there != account) {
            qInfo().noquote() << "omnuv: staying at" << m_coreUrl << "- its own address" << canonical << "answered" << code;
            return;
        }
        const QString was = m_coreUrl;
        m_coreUrl = canonical;
        saveToken(m_token);
        remember(QStringLiteral("omnuv/coreUrl"), m_coreUrl);
        updateNetworkScope();
        emit coreUrlChanged();
        setStatus(tr("Using %1, this Omnuv's own address, instead of %2.").arg(canonical, was));
    });
}

void OmnuvSession::invalidateContext()
{
    m_tunnel->clearOperationError();
    setCredentialWarning(QString());
    setReadProblem(true, QString());
    setReadProblem(false, QString());
    finishPairing();
    if (m_tunnel->busy()) m_tunnel->giveUp(tr("Network enrollment was cancelled because the account or project changed."));
    ++m_context;
    emit connectionContextChanged();
    ++m_identityRequest;
    ++m_machineRequest;
    m_identityPending = false;
    m_identityKnown = false;
    m_accountEmail.clear();
    m_accountId.clear();
    updateNetworkScope();
    m_projectNames.clear();
    m_projectIds.clear();
    m_machines->clear();
    m_estate->clear();
    emit projectsChanged();
}

// **Off the buyer's network when the person signs out (BUYER-13).** Signing
// out, or having access taken back, left this device a peer of the project's
// private network: the tunnel stayed up and anyone at the keyboard could still
// reach the buyer's machines without being signed in. The only caller of
// `stopMembership` was enrollment cleanup.
//
// The device is removed at Core when Core can still be asked — the token is
// needed for that, so this runs before it is dropped — and the membership is
// stopped here always, without waiting: signing out on this device never
// depends on the network.
void OmnuvSession::leaveNetwork(bool askCore)
{
    const auto membership = m_tunnel->membership();
    if (membership.isEmpty()) return;
    const auto network = membership.value("network_id").toString();
    const auto device = membership.value("device_id").toString();
    if (askCore && !m_token.isEmpty() && !m_fixture && OmnuvCredentials::secureCore(m_coreUrl)
        && !network.isEmpty() && !device.isEmpty()) {
        auto reply = m_net.deleteResource(request(QStringLiteral("/v1/networks/%1/devices/%2").arg(network, device), true));
        connect(reply, &QNetworkReply::finished, reply, [reply]() {
            reply->deleteLater();
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code == 204 || code == 404) qInfo("omnuv: this device was removed from its network at Core");
            else qWarning().noquote() << "omnuv: Core did not remove this device from its network:" << code << reply->errorString();
        });
    }
    m_tunnel->stopMembership(membership);
}

void OmnuvSession::signOut()
{
    // **Revoked at Core, not only forgotten here (H5, 22 September 2026).**
    // Deleting the token locally left it valid at Core, with no expiry, so any
    // copy of it still worked. Asked before the token is dropped, and not
    // waited for: signing out on this device never depends on the network.
    // A fixture's token is not a credential, and no request goes to a fixture.
    // The network first, while the token that can ask Core still exists.
    leaveNetwork(true);
    QNetworkReply* revocation = (!m_token.isEmpty() && !m_fixture && OmnuvCredentials::secureCore(m_coreUrl))
        ? m_net.deleteResource(request(QStringLiteral("/v1/devices/tokens/current"), true))
        : nullptr;
    invalidateContext();
    // Both the credential store and any file an older version left behind.
    // Signing out must not leave a token anywhere.
    if (!m_clearToken()) {
        setCredentialWarning(tr("The saved sign-in could not be removed. It may return when you reopen the app. Revoke this device in the console and retry signing out."));
    }
    m_token.clear();
    m_identityKnown = false;
    m_refreshTimer.stop();
    m_machines->clear();
    m_estate->clear();
    clearPending();
    emit signedInChanged();
    setStatus(revocation ? tr("Signing out…")
                         : tr("Signed out on this device. Revoke it in the console to take the access back for good."));
    if (!revocation) return;
    const auto context = m_context;
    connect(revocation, &QNetworkReply::finished, this, [this, revocation, context]() {
        revocation->deleteLater();
        const int code = revocation->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // 401: already not a token, which is the outcome asked for.
        const bool revoked = code == 204 || code == 401;
        if (revoked) qInfo("omnuv: the token was revoked at Core");
        else qWarning().noquote() << "omnuv: Core did not revoke the token:" << code << revocation->errorString();
        // A later sign-in has its own status; this one is no longer news.
        if (context != m_context || signedIn()) return;
        setStatus(revoked
            ? tr("Signed out. This device's access was revoked.")
            : tr("Signed out on this device, but Omnuv could not revoke its access. Revoke it in the console, under your account's applications."));
    });
}

// Revoked in the console, or expired. Say so and stop pretending to be
// signed in, rather than retrying a token that is dead.
void OmnuvSession::accessTakenBack()
{
    // The token is dead, so Core cannot be asked; the membership still stops.
    leaveNetwork(false);
    invalidateContext();
    clearPending();
    if (!m_clearToken()) {
        setCredentialWarning(tr("The expired or revoked sign-in could not be removed from this device. Retry signing out to remove the saved copy."));
    }
    m_token.clear();
    m_identityKnown = false;
    m_refreshTimer.stop();
    m_machines->clear();
    m_estate->clear();
    emit signedInChanged();
    setStatus(tr("This device's access was taken back. Sign in again."));
}

void OmnuvSession::refresh(bool everything)
{
    if (!signedIn()) {
        return;
    }

    // **Who, before what.** Every read below names a project, and a request
    // made before `/v1/me` has answered names none — which Core reads as the
    // default project, and which for an organization with no project is a
    // 404. So the identity comes first, and is asked again on every tick
    // until it arrives: a `/v1/me` that failed once must not leave the window
    // empty for good.
    if (!m_identityKnown) {
        fetchIdentity();
        return;
    }

    // The estate follows the same tick, and only while somebody can see it:
    // the tray reads machines and nothing else. Before the project check,
    // because the organization's own reads need no project.
    if (m_windowVisible) {
        m_estate->refresh(everything);
    }

    // **No project, nothing to ask.** Core answers a project-scoped read for
    // an organization with none with 404, which used to arrive here as
    // "could not reach" about a server that had answered. Asked again when a
    // person refreshes or opens the window, because signing in to the console
    // is what creates a new workspace and this is how the window notices.
    if (noProject()) {
        if (everything) {
            fetchIdentity();
        }
        return;
    }

    const auto context = m_context;
    const auto sequence = ++m_machineRequest;
    QNetworkReply* reply = m_net.get(request(QStringLiteral("/v1/instances") + projectQuery(), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context, sequence]() {
        reply->deleteLater();
        if (context != m_context || sequence != m_machineRequest) return;

        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // **Only a 401 means the token is gone (H22).** A 403 is Core refusing
        // this request — a policy, a fenced project — with the token still
        // good, and it signed the person out. It is shown below, in Core's
        // words, like any other refusal.
        if (code == 401) {
            accessTakenBack();
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            // Core's sentence when Core answered; "could not reach" only when
            // nothing did. The two send a person to different places.
            const QString said = QJsonDocument::fromJson(reply->readAll()).object()
                                     .value(QStringLiteral("error")).toString().trimmed();
            setReadProblem(false, code == 0 ? tr("Could not reach %1.").arg(m_coreUrl)
                      : !said.isEmpty() ? tr("Omnuv could not list the machines: %1").arg(said)
                      : tr("Omnuv could not list the machines just now."));
            return;
        }

        QJsonParseError parsed;
        const auto doc = QJsonDocument::fromJson(reply->readAll(), &parsed);
        if (parsed.error != QJsonParseError::NoError || !validMachines(doc)) {
            setReadProblem(false, tr("Omnuv returned a machine list this version cannot read. Your previous machine view has been kept."));
            return;
        }
        const QJsonArray machines = doc.array();
        m_machines->replace(machines);
        setReadProblem(false, QString());
        setStatus(QString());

        // Logged because a person reporting "it says I have no machines" needs
        // this line to tell an empty account from a request that never arrived.
        // **What was asked for, not only what came back.** An empty list and a
        // query naming the wrong project produce the same count, and on
        // 16 September that cost three rounds of diagnosis on a machine that
        // was up the whole time: the client said `0 machine(s)` and nothing
        // said which project it had asked about, or how many it knew of.
        qInfo() << "omnuv: signed in," << machines.count() << "machine(s), project"
                << (m_projectId.isEmpty() ? QStringLiteral("(default)") : m_projectId)
                << "of" << m_projectIds.count();
    });
}

// An ordinary machine is reached with ssh, and every desktop starts a terminal
// its own way. Try the ones that exist, take the first that starts, and tell
// the caller if none did.
// **The names come from Core, so they are checked before any program sees
// them.** On Windows they reached `cmd /c start`, where `&` or `|` in a
// machine's name would run a command; on every platform a name starting with
// `-` reached ssh as an option, and `-oProxyCommand=` runs one. The same rules
// the omnuv:// handlers apply: a DNS name, and a login name.
bool OmnuvSession::sshTargetIsSafe(const QString& host, const QString& user)
{
    static const QRegularExpression label(QStringLiteral(
        "^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$"));
    static const QRegularExpression login(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]{0,31}$"));
    if (host.size() > 253 || !label.match(host).hasMatch()) return false;
    return user.isEmpty() || login.match(user).hasMatch();
}

bool OmnuvSession::openTerminal(const QString& host, const QString& user)
{
    if (!sshTargetIsSafe(host, user)) {
        qWarning().noquote() << "omnuv: not opening ssh to a name that is not a machine's:" << user << host;
        return false;
    }
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

QJsonObject OmnuvSession::networkScope() const
{
    if (!signedIn() || !m_identityKnown || m_accountId.isEmpty() || m_projectId.isEmpty()) return {};
    // One definition of a Core's identity, shared with the token's slot.
    return {{"core_url", tokenOrigin()}, {"account_id", m_accountId}, {"project_id", m_projectId}};
}

void OmnuvSession::updateNetworkScope()
{
    const auto scope = networkScope();
    if (scope == m_networkScope) return;
    m_networkScope = scope;
    ++m_enrollmentRequest;
    m_networkMovePrompt.clear(); m_networkMoveRevision.clear(); m_authorizedMoveRevision.clear();
    m_tunnel->setScope(scope);
    emit enrollmentChanged();
}

QJsonObject OmnuvSession::pendingEnrollment() const
{
    const auto scope = networkScope();
    if (scope.isEmpty()) return {};
    const auto records = m_enrollmentJournal.records();
    for (const auto& value : records) {
        const auto record = value.toObject(), membership = record.value("membership").toObject();
        if (record.value("cleanup_done").toBool()) continue;
        bool matches = true;
        for (auto it = scope.begin(); it != scope.end(); ++it)
            if (it.value() != membership.value(it.key())) matches = false;
        if (matches) return record;
    }
    return {};
}

QString OmnuvSession::enrollmentRecovery() const
{
    const auto record = pendingEnrollment();
    if (record.isEmpty()) return {};
    const auto membership = record.value("membership").toObject();
    return tr("Enrollment %1 is saved for this project. Retry reuses this attempt. If its key was already issued, revoke it before starting again. Closing the app keeps these recovery details.")
        .arg(membership.value("device_id").toString())
        + (record.value("unexpected_device_id").toString().isEmpty() ? QString()
            : tr(" An older server also returned device %1; cleanup must remove that device.").arg(record.value("unexpected_device_id").toString()));
}

void OmnuvSession::observeEnrollment()
{
    if (m_observingEnrollment || !m_tunnel->connected()) return;
    m_observingEnrollment = true;
    const auto record = pendingEnrollment();
    const auto membership = record.value("membership").toObject();
    if (!record.isEmpty() && record.value("unexpected_device_id").toString().isEmpty()
        && m_tunnel->membership() == membership) {
        if (!m_enrollmentJournal.remove(OmnuvEnrollmentJournal::key(membership)))
            m_tunnel->giveUp(tr("The network joined, but its recovery record could not be cleared. Retry after fixing local settings storage."));
        emit enrollmentChanged();
    }
    m_observingEnrollment = false;
}

void OmnuvSession::confirmNetworkMove()
{
    if (m_networkMovePrompt.isEmpty() || m_networkMoveContext != m_context) return;
    m_authorizedMoveRevision = m_networkMoveRevision;
    m_networkMovePrompt.clear();
    emit enrollmentChanged();
    m_tunnel->join();
}

void OmnuvSession::cancelNetworkMove()
{
    m_networkMovePrompt.clear(); m_networkMoveRevision.clear(); m_authorizedMoveRevision.clear();
    emit enrollmentChanged();
}

void OmnuvSession::revokePendingEnrollment()
{
    if (m_cleanupInFlight) return;
    const auto record = pendingEnrollment();
    if (record.isEmpty()) return;
    const auto membership = record.value("membership").toObject();
    const auto key = OmnuvEnrollmentJournal::key(membership);
    ++m_enrollmentRequest; // A late successful POST must never start a cancelled enrollment.
    const auto context = m_context;
    m_cleanupInFlight = true;
    emit enrollmentChanged();
    m_tunnel->beginOperation(tr("Revoking this enrollment…"));
    auto reply = m_net.deleteResource(request(QStringLiteral("/v1/networks/%1/devices/%2")
        .arg(membership.value("network_id").toString(), membership.value("device_id").toString()), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply, record, membership, key, context]() mutable {
        reply->deleteLater();
        m_cleanupInFlight = false;
        emit enrollmentChanged();
        const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto cause = QJsonDocument::fromJson(reply->readAll()).object().value("error").toString();
        if (reply->error() != QNetworkReply::NoError || code != 204) {
            if (context == m_context) m_tunnel->giveUp(tr("Enrollment cleanup is not confirmed (HTTP %1): %2. The saved attempt has been kept; update the server if this operation is unsupported.")
                .arg(code).arg(cause.isEmpty() ? reply->errorString() : cause));
            return;
        }
        if (context != m_context) return; // retry cleanup in the original account/deployment
        // A cancellation fence is now durable at Core, including when DELETE
        // arrived before the original create. Stop only that exact membership.
        if (!m_tunnel->stopMembership(membership)) return;
        const auto unexpected = record.value("unexpected_device_id").toString();
        if (!unexpected.isEmpty()) {
            auto old = m_net.deleteResource(request(QStringLiteral("/v1/devices/%1").arg(unexpected), true));
            connect(old, &QNetworkReply::finished, this, [this, old, record, key, context]() mutable {
                old->deleteLater();
                if (old->error() != QNetworkReply::NoError) {
                    if (context == m_context) m_tunnel->giveUp(tr("The older server's extra device could not be revoked. Recovery details have been kept."));
                    return;
                }
                auto done = record; done["cleanup_done"] = true;
                if (!m_enrollmentJournal.put(key, done)) {
                    if (context == m_context) m_tunnel->giveUp(tr("Cleanup succeeded, but could not be saved locally. Retry cleanup."));
                } else if (context == m_context) m_tunnel->giveUp(tr("Enrollment revoked. Join this device to start a new attempt."));
                emit enrollmentChanged();
            });
            return;
        }
        auto done = record; done["cleanup_done"] = true;
        if (!m_enrollmentJournal.put(key, done)) {
            if (context == m_context) m_tunnel->giveUp(tr("Cleanup succeeded, but could not be saved locally. Retry cleanup."));
        } else if (context == m_context) m_tunnel->giveUp(tr("Enrollment revoked. Join this device to start a new attempt."));
        emit enrollmentChanged();
    });
}

// Establish the scope before either resuming an identity or creating a key.
// **Not running is not out of date (H25).** Join said "update the network
// service" whenever the membership could not be read, and the usual reason is
// that nothing answered at all: the service is stopped, and updating it is the
// wrong remedy. Nothing answering and an answer this version cannot read are
// told apart, and each names what to do.
QString OmnuvSession::tunnelServiceProblem(const QString& consequence) const
{
    if (!m_tunnel->serviceAnswered()) {
#ifdef Q_OS_WIN
        return tr("The Omnuv network service (OnvTunnel) is not running. Start it from Services, or reinstall Omnuv, then join again. %1").arg(consequence);
#else
        return tr("The Omnuv network service is not running. Start Omnuv again, then join again. %1").arg(consequence);
#endif
    }
    return tr("Update the Omnuv network service before joining: this version cannot read its answer. %1").arg(consequence);
}

void OmnuvSession::fetchDeviceKey()
{
    if (!signedIn()) {
        m_tunnel->giveUp(tr("Sign in first, so this device can be added to your network.")); return;
    }
    if (!m_identityKnown || m_projectId.isEmpty()) {
        m_tunnel->giveUp(tr("Choose a project before joining this device to its network.")); return;
    }
    if (m_accountId.isEmpty()) {
        m_tunnel->giveUp(tr("Your server did not provide a stable account identity. Update it before enrolling this device.")); return;
    }
    const auto scope = networkScope();
    m_tunnel->setScope(scope);
    if (!m_tunnel->readMembership()) {
        m_tunnel->giveUp(tunnelServiceProblem(tr("Its existing identity has been kept."))); return;
    }
    bool valid;
    m_enrollmentJournal.records(&valid);
    if (!valid) {
        m_tunnel->giveUp(tr("Saved enrollment recovery details could not be read. Repair local settings before starting another attempt.")); return;
    }
    const auto context = m_context, operation = ++m_enrollmentRequest;
    const QString project = m_projectId;
    auto reply = m_net.get(request(QStringLiteral("/v1/networks") + projectQuery(), true));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context, operation, project, scope]() {
        reply->deleteLater();
        if (context != m_context || operation != m_enrollmentRequest || project != m_projectId) return;
        const auto bytes = reply->readAll();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            const auto cause = QJsonDocument::fromJson(bytes).object().value("error").toString().trimmed();
            const auto problem = code == 0 ? tr("Could not reach Omnuv to read your network: %1").arg(reply->errorString())
                : tr("Omnuv could not read your network (HTTP %1): %2").arg(code).arg(cause.isEmpty() ? reply->errorString() : cause);
            if (code == 401) { accessTakenBack(); setStatus(tr("Sign in again. %1").arg(problem)); }
            m_tunnel->giveUp(problem); return;
        }
        QJsonParseError parsed;
        const auto document = QJsonDocument::fromJson(bytes, &parsed);
        const auto networks = document.array();
        if (parsed.error != QJsonParseError::NoError || !document.isArray()
            || (!networks.isEmpty() && (!networks.first().isObject() || !nonemptyString(networks.first().toObject(), "id")))) {
            m_tunnel->giveUp(tr("Omnuv returned network details this version cannot read.")); return;
        }
        if (networks.isEmpty()) {
            m_tunnel->giveUp(tr("This project's network is not ready yet. Try joining again after it is ready.")); return;
        }
        auto target = scope;
        target["network_id"] = networks.first().toObject().value("id");
        const auto journalKey = OmnuvEnrollmentJournal::key(target);
        const auto previous = m_enrollmentJournal.records().value(journalKey).toObject();
        const auto previousMembership = previous.value("membership").toObject();
        if (!m_tunnel->readMembership()) {
            m_tunnel->giveUp(tunnelServiceProblem(tr("No enrollment was created."))); return;
        }
        const auto current = m_tunnel->membership();
        const bool previouslyRevoked = previous.value("cleanup_done").toBool()
            && current == previousMembership;
        if (m_tunnel->membershipMatches(target) && !previouslyRevoked) {
            if (!m_tunnel->membershipVerified()) {
                m_tunnel->giveUp(tr("This enrollment has not completed. Its saved attempt must be recovered or revoked before creating another.")); return;
            }
            m_tunnel->resumeMembership(current);
            if (!m_tunnel->operationError().isEmpty() && previous.isEmpty()) {
                m_enrollmentJournal.put(journalKey, {{"membership", current}});
                emit enrollmentChanged();
            }
            return;
        }
        const auto revision = m_tunnel->membershipRevision();
        if ((m_tunnel->hasIdentity() || !current.isEmpty()) && !previouslyRevoked
            && m_authorizedMoveRevision != revision) {
            m_networkMoveRevision = revision; m_networkMoveContext = m_context;
            m_networkMovePrompt = tr("Move this device to %1 at %2? This disconnects its current network. Before confirming, revoke the old device in its original console. Current membership: %3. The current identity is kept if you cancel.")
                .arg(projectName(), m_coreUrl, current.isEmpty() ? tr("unverified; its original account must identify it")
                    : tr("device %1, project %2, account %3 at %4").arg(current.value("device_id").toString(), current.value("project_id").toString(), current.value("account_id").toString(), current.value("core_url").toString()));
            m_tunnel->giveUp(tr("Confirm the network move after revoking the old membership."));
            emit enrollmentChanged(); return;
        }
        if (!pendingEnrollment().isEmpty()
            && pendingEnrollment().value("membership").toObject().value("network_id") != target.value("network_id")) {
            m_tunnel->giveUp(tr("Recover or revoke the saved enrollment for this project's previous network first.")); return;
        }
        QJsonObject record;
        if (!m_enrollmentJournal.reserve(target, &record)) {
            m_tunnel->giveUp(tr("The enrollment attempt could not be saved. No device was created.")); return;
        }
        const auto membership = record.value("membership").toObject();
        const auto attempt = membership.value("device_id").toString();
        if (!record.value("unexpected_device_id").toString().isEmpty()) {
            m_tunnel->giveUp(tr("An older server returned a different enrollment ID. Revoke the saved attempt before retrying.")); return;
        }
        emit enrollmentChanged();
        const QJsonObject body {{"name", QSysInfo::machineHostName()}, {"attempt_id", attempt}};
        auto add = m_net.post(request(QStringLiteral("/v1/networks/%1/devices").arg(target.value("network_id").toString()), true),
            QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(add, &QNetworkReply::finished, this, [this, add, context, operation, project, membership, attempt, journalKey, record, revision]() mutable {
            add->deleteLater();
            const auto bytes = add->readAll();
            const auto document = QJsonDocument::fromJson(bytes);
            const auto o = document.object();
            const auto returnedId = o.value("id").toString();
            // Even an obsolete response may identify an old server's side effect.
            // Keep that ID for cleanup without using its secret or new context.
            if (!returnedId.isEmpty() && returnedId != attempt) {
                auto updated = record; updated["unexpected_device_id"] = returnedId;
                if (!m_enrollmentJournal.put(journalKey, updated))
                    qWarning().noquote() << "omnuv: enrollment cleanup ID could not be saved:" << returnedId;
                emit enrollmentChanged();
            }
            if (context != m_context || operation != m_enrollmentRequest || project != m_projectId) return;
            const int code = add->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (add->error() != QNetworkReply::NoError) {
                const auto cause = o.value("error").toString().trimmed();
                const auto problem = code == 0 ? tr("Could not reach Omnuv to add this device: %1").arg(add->errorString())
                    : tr("Omnuv could not add this device (HTTP %1): %2").arg(code).arg(cause.isEmpty() ? add->errorString() : cause);
                if (code == 401) { accessTakenBack(); setStatus(tr("Sign in again. %1").arg(problem)); }
                m_tunnel->giveUp(problem + tr(" Recovery attempt: %1.").arg(attempt)); return;
            }
            if (returnedId != attempt) {
                m_tunnel->giveUp(tr("This server does not support recoverable enrollment. Expected device %1; returned %2. The current network identity was kept. Revoke the saved enrollment before updating and retrying.")
                    .arg(attempt, returnedId.isEmpty() ? tr("no device ID") : returnedId)); return;
            }
            if (!document.isObject() || !nonemptyString(o, "setup_key") || !nonemptyString(o, "command")) {
                m_tunnel->giveUp(tr("Omnuv returned enrollment details this version cannot read. Revoke the saved enrollment before trying again.")); return;
            }
            // Core's own field where it sends one; parsed out of the command
            // only for a Core from before 23 September 2026.
            const auto command = o.value("command").toString();
            const auto url = nonemptyString(o, "management_url") ? o.value("management_url").toString()
                : command.section(QStringLiteral("--management-url "), 1, 1).section(QLatin1Char(' '), 0, 0);
            const QUrl management(url);
            // The daemon's rule (membership.go `origin`): https, or http to
            // this machine alone. Any http used to pass here and be refused
            // by the daemon, after the key had already been spent.
            if (!management.isValid() || management.host().isEmpty()
                || !OmnuvCredentials::secureCore(url)) {
                m_tunnel->giveUp(tr("Your network did not provide a usable address to join. Revoke the saved enrollment before trying again.")); return;
            }
            m_authorizedMoveRevision.clear();
            m_tunnel->enrolMembership(url, o.value("setup_key").toString(), membership, revision);
        });
    });
}

// Carry identity across every asynchronous step; a model row is only a view.
QVariantMap OmnuvSession::connectionTarget(int row) const
{
    if (!signedIn() || m_projectId.isEmpty() || m_machines->idAt(row).isEmpty()
        || m_machines->hostAt(row).isEmpty()) return {};
    return {{"id", m_machines->idAt(row)}, {"host", m_machines->hostAt(row)},
            {"project", m_projectId}, {"context", QString::number(m_context)}};
}

int OmnuvSession::targetRow(const QVariantMap& target) const
{
    if (!signedIn() || target.value("project").toString() != m_projectId
        || target.value("context").toString() != QString::number(m_context)
        || target.value("id").toString().isEmpty()) return -1;
    for (int row = 0; row < m_machines->rowCount(); ++row) {
        if (m_machines->idAt(row) == target.value("id").toString()
            && m_machines->hostAt(row) == target.value("host").toString()) return row;
    }
    return -1;
}

void OmnuvSession::finishPairing(bool delivered)
{
    m_pairing->cancel();
    auto scope = m_pairScope;
    m_pairScope = nullptr;
    delete scope;
    if (!m_claimDeployment.isEmpty() && signedIn()) {
        auto reply = m_net.post(request(QStringLiteral("/v1/deployments/%1/stream-credentials/complete")
            .arg(m_claimDeployment), true),
            QJsonDocument(QJsonObject{{"attempt_id", m_claimAttempt}, {"delivered", delivered}}).toJson());
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
        // A lost acknowledgement is bounded by Core's non-renewable expiry.
    }
    m_claimDeployment.clear();
    m_claimAttempt.clear();
}

void OmnuvSession::deliverPin(const QVariantMap& target, const QString& pin)
{
    finishPairing();
    if (targetRow(target) < 0) return;
    m_pairScope = new QObject(this);
    m_claimAttempt = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // End locally before the server's five-minute claim expires.
    QTimer::singleShot(240000, m_pairScope, [this]() {
        finishPairing();
        emit pairingFailed(tr("Automatic pairing timed out. Pair by hand or try again."), QString());
    });
    // A refresh can remove the instance or change its address without changing project.
    connect(m_machines, &MachineModel::countChanged, m_pairScope, [this, target]() {
        if (targetRow(target) < 0) { finishPairing(); emit connectionContextChanged(); }
    });
    auto reply = m_net.get(request(QStringLiteral("/v1/deployments") + projectQuery(), true));
    connect(m_pairScope, &QObject::destroyed, reply, [reply]() { reply->disconnect(); reply->abort(); reply->deleteLater(); });
    connect(reply, &QNetworkReply::finished, m_pairScope, [this, reply, target, pin]() {
        reply->deleteLater();
        if (targetRow(target) < 0) { finishPairing(); return; }
        if (reply->error() != QNetworkReply::NoError) {
            m_pairing->giveUp(tr("Omnuv could not be reached to collect this machine's login.")); return;
        }
        for (const auto& value : QJsonDocument::fromJson(reply->readAll()).array()) {
            const auto d = value.toObject();
            if (d["instance_id"].toString() == target.value("id").toString()) {
                m_claimDeployment = d["id"].toString(); break;
            }
        }
        if (m_claimDeployment.isEmpty()) {
            m_pairing->giveUp(tr("This machine has no automatic pairing login. Pair it by hand.")); return;
        }
        collectStreamLogin(target, pin, kLoginTries);
    });
}

void OmnuvSession::collectStreamLogin(const QVariantMap& target, const QString& pin, int triesLeft)
{
    if (!m_pairScope || targetRow(target) < 0) { finishPairing(); return; }
    auto reply = m_net.post(request(QStringLiteral("/v1/deployments/%1/stream-credentials/claim")
        .arg(m_claimDeployment), true), QJsonDocument(QJsonObject{{"attempt_id",m_claimAttempt}}).toJson());
    connect(m_pairScope, &QObject::destroyed, reply, [reply]() { reply->disconnect(); reply->abort(); reply->deleteLater(); });
    connect(reply, &QNetworkReply::finished, m_pairScope, [this, reply, target, pin, triesLeft]() {
        reply->deleteLater();
        if (targetRow(target) < 0) { finishPairing(); return; }
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto o = QJsonDocument::fromJson(reply->readAll()).object();
        const auto status = o["status"].toString();
        // A timeout may follow a committed claim. Retry its SAME attempt id.
        if ((code == 0 || code >= 500 || (code == 200 && status == "waiting")) && triesLeft > 1) {
            QTimer::singleShot(kLoginIntervalMs, m_pairScope, [this, target, pin, triesLeft]() {
                collectStreamLogin(target, pin, triesLeft - 1);
            });
            return;
        }
        if (code != 200 || status != "ready" || o["user"].toString().isEmpty() || o["password"].toString().isEmpty()) {
            m_pairing->giveUp(tr("Automatic pairing is unavailable. You can pair this machine by hand."),
                             tr("Credential claim returned %1 (%2).").arg(code).arg(status));
            return;
        }
        m_pairing->deliver(target.value("host").toString(), pin, QSysInfo::machineHostName(),
                          m_tunnel->address(), o["user"].toString(), o["password"].toString());
    });
}

void OmnuvSession::watchPairing(QObject* object)
{
    auto manager = qobject_cast<ComputerManager*>(object);
    if (!manager || m_pairManager == manager) return;
    if (m_pairManager) disconnect(m_pairManager, nullptr, this, nullptr);
    m_pairManager = manager;
    connect(manager, &ComputerManager::pairingCompleted, this,
        [this](NvComputer* computer, const QString& error) {
            emit hostPairingFinished(computer->manualAddress.address(), error.isEmpty() ? QVariant() : QVariant(error));
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
    // Fixture launches always show their window and never consume first run.
    if (m_fixture) return false;
    QSettings settings;
    const bool ranBefore = settings.value(QStringLiteral("omnuv/hasRunBefore"), false).toBool();
    settings.setValue(QStringLiteral("omnuv/hasRunBefore"), true);
    settings.sync();

    return ranBefore && m_autostart != nullptr && m_autostart->enabled();
}

void OmnuvSession::setWindowVisible(bool visible)
{
    m_windowVisible = visible;
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
        refresh(true);
    }
}

// Registered here rather than in main.cpp, which belongs to upstream. This
// runs before the QML engine is built, which is all registration needs.
static void registerOmnuvTypes()
{
    qmlRegisterSingletonType<OmnuvSession>("Omnuv", 1, 0, "Omnuv",
                                           [](QQmlEngine*, QJSEngine*) -> QObject* {
                                               auto* session = new OmnuvSession();
                                               // The title bar says the product's name. The
                                               // *application* name stays `OmnuvClient` —
                                               // settings, the credential target and the log
                                               // directory are keyed on it — and with no display
                                               // name set Qt titled the window with that.
                                               QGuiApplication::setApplicationDisplayName(QStringLiteral("Omnuv"));
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
    qmlRegisterUncreatableType<OmnuvEstate>("Omnuv", 1, 0, "OmnuvEstate",
                                            QStringLiteral("The estate comes from Omnuv.estate"));
    qmlRegisterUncreatableType<OmnuvRead>("Omnuv", 1, 0, "OmnuvRead",
                                          QStringLiteral("Reads come from Omnuv.estate"));

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
