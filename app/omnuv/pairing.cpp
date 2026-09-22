#include "pairing.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

// The streaming host's own web API, on its own port.
//
// **Verified, not remembered.** Sunshine is pinned by the platform's
// `deployment/ansible/group_vars/proxmox.yml` at tag v2026.906.222525, and
// every fact below was read out of that tag's source:
//
//   src/confighttp.h:29     PORT_HTTPS = 1, a *offset* from the configured
//                           base port, not a literal. The base defaults to
//                           47989 (src/config.cpp, `port`), and
//                           src/network.cpp:224 map_port() adds the offset,
//                           so the web API is 47990. A recipe that moved the
//                           base port would move this with it — the constant
//                           below is the one place that would have to change,
//                           which is why it is a constant.
//   src/confighttp.cpp:2299 server.resource["^/api/pin$"]["POST"] = savePin
//   src/confighttp.cpp:2298 ...["GET"] = getPendingPairings
//   src/confighttp.cpp:1790 savePin() reads three fields from the body:
//                           "pairing_id", "pin", "name". Not one.
//   src/nvhttp.cpp:1083     nvhttp::pin() returns bool; savePin puts it in
//                           the response as {"status": true|false}.
static const int kWebPort = 47990;

// How long to wait for the machine to answer at all. Long enough for a slow
// overlay hop, short enough that a machine which is not there fails while the
// person is still looking at the screen they pressed Play on.
static const int kRequestTimeoutMs = 10 * 1000;

// The pairing request this client just made has to arrive at the machine and
// be registered there before the machine can name it. That is normally done
// before the two Core round trips this path already made, but it is a race
// either way, so look again a few times rather than concluding on the first
// empty answer.
static const int kFindTries = 120;
static const int kFindIntervalMs = 500;

OmnuvPairing::OmnuvPairing(QNetworkAccessManager* net, QObject* parent)
    : QObject(parent), m_net(net)
{
}

void OmnuvPairing::cancel()
{
    delete m_scope;
    m_scope = nullptr;
}

void OmnuvPairing::giveUp(const QString& why, const QString& detail)
{
    emit failed(why, detail);
}

// **The certificate cannot be checked, and this says so rather than pretending
// otherwise.**
//
// Sunshine serves its web API with the same self-signed certificate it serves
// GameStream with (src/confighttp.cpp:2244, `https_server_t server
// {config::nvhttp.cert, config::nvhttp.pkey}`) — a key pair the machine
// generated for itself at install. Nothing on this device has ever seen it:
// Moonlight learns it during pairing, from the `plaincert` in stage one
// (app/backend/nvpairingmanager.cpp:245), and persists it only once pairing
// has *finished* (:370, then ComputerManager::saveHost). We are here before
// that, which is the whole point — so there is no pin to compare against and
// no authority to appeal to.
//
// What the confidentiality of the login therefore rests on is the overlay, and
// only the overlay: the name resolves inside WireGuard, the key for that peer
// is distributed to this project's peers and nothing else, and under topology
// v2 there is no plaintext hop on the way — not even on the provider's own
// bridge. That is the same property the stream itself rests on, so this adds
// no exposure that pressing Play does not already accept.
//
// Narrow rather than blanket, because "self-signed" and "broken" are different
// answers: the errors below are the ones an untrusted self-signed leaf
// actually produces, and anything else — expired, revoked, a bad signature —
// still fails the request.
//
// ponytail: an untrusted first contact. The upgrade is trust-on-first-use —
// remember the leaf here and refuse a different one later — which is only
// worth building once a machine's Sunshine state outlives a redeploy.
void OmnuvPairing::acceptSelfSigned(QNetworkReply* reply, const QList<QSslError>& errors)
{
    // Names checked against Qt 6.11's own `src/network/ssl/qsslerror.h`; a
    // switch rather than a set so the compiler is the one that checks them.
    const auto selfSignedOnly = [](QSslError::SslError error) {
        switch (error) {
        case QSslError::SelfSignedCertificate:
        case QSslError::SelfSignedCertificateInChain:
        case QSslError::CertificateUntrusted:
        case QSslError::UnableToGetLocalIssuerCertificate:
        case QSslError::UnableToVerifyFirstCertificate:
        // The certificate names the machine's own Sunshine host, never the
        // marketplace name we dialled, so this one is structural.
        case QSslError::HostNameMismatch:
            return true;
        default:
            return false;
        }
    };

    for (const QSslError& error : errors) {
        if (!selfSignedOnly(error.error())) {
            return;
        }
    }
    reply->ignoreSslErrors(errors);
}

QNetworkRequest OmnuvPairing::webRequest(const QString& host,
                                         const QString& user,
                                         const QString& password) const
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(host);
    url.setPort(kWebPort);
    url.setPath(QStringLiteral("/api/pin"));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(kRequestTimeoutMs);

    // HTTP Basic, which is what src/confighttp.cpp:562 authenticate() reads:
    // it takes the "authorization" header, strips "Basic ", base64-decodes it,
    // splits on the first colon and compares the name case-insensitively and
    // the password as hex(hash(password + salt)).
    //
    // The login goes in the header and never in the URL. A URL with userinfo
    // in it reaches `reply->url()`, which is the one part of a request that
    // does end up in messages people paste into support threads.
    const QByteArray basic = (user + QLatin1Char(':') + password).toUtf8().toBase64();
    req.setRawHeader("authorization", QByteArrayLiteral("Basic ") + basic);

    // **Sending neither Origin nor Referer is load-bearing.**
    // src/confighttp.cpp:763 validate_csrf_token() returns true early when
    // both headers are absent, on the stated reasoning that a non-browser
    // client cannot be the subject of a cross-site request. A default
    // QNetworkAccessManager sends neither, so no CSRF token round trip is
    // needed — and anything that later adds one turns pairing into a 400
    // reading "Missing CSRF token". Asserted rather than commented, because a
    // comment would not have noticed.
    Q_ASSERT(!req.hasRawHeader("Origin") && !req.hasRawHeader("Referer"));

    return req;
}

QString OmnuvPairing::sentenceFor(QNetworkReply* reply, const QString& apiError)
{
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    switch (code) {
    case 401:
        // authenticate() failed: the name or the password did not match what
        // the machine was started with.
        return QObject::tr("The machine would not accept the one-time login it published. "
                           "That login is spent once and cannot be reissued, so this "
                           "pairing has to be done by hand.");
    case 403:
        // net::from_address(remote) > origin_web_ui_allowed, which defaults to
        // "lan" and counts 100.64.0.0/10 as LAN (src/network.cpp:33). Reaching
        // it from anywhere else means the connection did not go over the
        // project network.
        return QObject::tr("The machine refused the request because it did not arrive over "
                           "your private network. Join this device to the network and try "
                           "again.");
    case 400:
        // bad_request(): the body was wrong, or CSRF blocked it. Either way it
        // is ours, not the person's, so say what the machine said.
        return apiError.isEmpty()
                   ? QObject::tr("The machine did not understand the request.")
                   : QObject::tr("The machine refused the request: %1").arg(apiError);
    default:
        break;
    }

    if (reply->error() != QNetworkReply::NoError) {
        return QObject::tr("The machine's setup page did not answer. It is reachable for "
                           "streaming but not for pairing, which usually means it is still "
                           "starting.");
    }
    return QObject::tr("The machine answered in a way this device did not expect.");
}

void OmnuvPairing::deliver(const QString& host,
                           const QString& pin,
                           const QString& clientName,
                           const QString& fromAddress,
                           const QString& user,
                           const QString& password)
{
    cancel();
    m_scope = new QObject(this);
    findPendingPairing(host, pin, clientName, fromAddress, user, password, kFindTries);
}

// **The order is the machine's, and it is the opposite of the obvious one.**
//
// A PIN cannot be delivered before there is something to deliver it to:
// savePin() refuses any `pairing_id` that is not one of the requests currently
// waiting (src/nvhttp.cpp:1083, which looks the id up among sessions still in
// PAIR_PHASE::NONE), and a request only starts waiting when the client has
// made it. Sunshine holds that first `getservercert` response open until a PIN
// arrives or five minutes pass (src/nvhttp.cpp:955 stores the response on the
// session; nvhttp.h:68 PAIRING_SESSION_TIMEOUT).
//
// So the client pairs first and asks afterwards, and this is the asking.
void OmnuvPairing::findPendingPairing(const QString& host,
                                      const QString& pin,
                                      const QString& clientName,
                                      const QString& fromAddress,
                                      const QString& user,
                                      const QString& password,
                                      int triesLeft)
{
    QNetworkReply* reply = m_net->get(webRequest(host, user, password));
    connect(m_scope, &QObject::destroyed, reply, [reply]() { reply->disconnect(); reply->abort(); reply->deleteLater(); });
    connect(reply, &QNetworkReply::sslErrors, m_scope, [reply](const QList<QSslError>& errors) {
        acceptSelfSigned(reply, errors);
    });

    connect(reply, &QNetworkReply::finished, m_scope,
            [this, reply, host, pin, clientName, fromAddress, user, password, triesLeft]() {
        reply->deleteLater();

        const QByteArray body = reply->readAll();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject o = QJsonDocument::fromJson(body).object();

        if ((code == 0 || code >= 500) && triesLeft > 1) {
            QTimer::singleShot(kFindIntervalMs, m_scope,
                [this, host, pin, clientName, fromAddress, user, password, triesLeft]() {
                    findPendingPairing(host, pin, clientName, fromAddress, user, password, triesLeft - 1);
                });
            return;
        }
        if (code != 200) {
            emit failed(sentenceFor(reply, o[QStringLiteral("error")].toString()),
                        tr("GET /api/pin returned %1.").arg(code == 0 ? reply->errorString()
                                                                      : QString::number(code)));
            return;
        }

        // src/confighttp.cpp:1713 getPendingPairings(): {"pairings":[{id,name,address}]}.
        const QJsonArray pairings = o[QStringLiteral("pairings")].toArray();

        // Ours, by the address the machine saw us arrive from. Every client
        // reports the same device name — Moonlight hard-codes `devicename=roth`
        // (app/backend/nvpairingmanager.cpp:235) — so the name cannot tell two
        // apart and the address is the only thing that can.
        QStringList ids;
        for (const QJsonValue& value : pairings) {
            const QJsonObject entry = value.toObject();
            if (fromAddress.isEmpty()
                || entry[QStringLiteral("address")].toString() == fromAddress) {
                ids << entry[QStringLiteral("id")].toString();
            }
        }
        // The machine may see this device at an address the tunnel does not
        // report. Rather than refuse, fall back to the whole list: picking the
        // wrong one costs a failed pairing, never a wrong one — the PIN
        // derives the key the *requesting* client used, so another device's
        // session cannot be completed with ours.
        if (ids.isEmpty() && !pairings.isEmpty()) {
            for (const QJsonValue& value : pairings) {
                ids << value.toObject()[QStringLiteral("id")].toString();
            }
        }

        if (ids.isEmpty()) {
            if (triesLeft > 1) {
                QTimer::singleShot(kFindIntervalMs, m_scope,
                                   [this, host, pin, clientName, fromAddress, user, password, triesLeft]() {
                    findPendingPairing(host, pin, clientName, fromAddress, user, password, triesLeft - 1);
                });
                return;
            }
            emit failed(tr("The machine is not waiting for a device to pair with it. "
                           "Its streaming host may have restarted while this was going on."),
                        tr("GET /api/pin listed no pending pairing after %1 attempts.")
                            .arg(kFindTries));
            return;
        }

        if (ids.count() > 1) {
            emit failed(tr("More than one device is pairing with this machine right now, and "
                           "this one cannot tell which request is its own. Finish the other "
                           "one first, or pair by hand."),
                        tr("GET /api/pin listed %1 pending pairings.").arg(ids.count()));
            return;
        }

        sendPin(host, ids.first(), pin, clientName, user, password);
    });
}

void OmnuvPairing::sendPin(const QString& host,
                           const QString& pairingId,
                           const QString& pin,
                           const QString& clientName,
                           const QString& user,
                           const QString& password)
{
    // src/nvhttp.cpp:601-615 validates all three before doing anything: the id
    // is exactly 32 hex characters, the PIN exactly four digits, and the name
    // between 1 and 128 *bytes*. A host name is neither empty nor that long in
    // practice, but the machine rejects the whole request over a label, so
    // fall back to one that cannot be refused rather than finding out.
    const QByteArray utf8 = clientName.toUtf8();
    const QString name = (utf8.isEmpty() || utf8.size() > 128) ? QStringLiteral("Omnuv")
                                                               : clientName;

    const QJsonObject body {
        { QStringLiteral("pairing_id"), pairingId },
        { QStringLiteral("pin"), pin },
        { QStringLiteral("name"), name },
    };

    QNetworkReply* reply = m_net->post(webRequest(host, user, password),
                                       QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_scope, &QObject::destroyed, reply, [reply]() { reply->disconnect(); reply->abort(); reply->deleteLater(); });
    connect(reply, &QNetworkReply::sslErrors, m_scope, [reply](const QList<QSslError>& errors) {
        acceptSelfSigned(reply, errors);
    });

    connect(reply, &QNetworkReply::finished, m_scope, [this, reply]() {
        reply->deleteLater();

        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();

        if (code == 200 && o[QStringLiteral("status")].toBool()) {
            emit delivered();
            return;
        }

        if (code == 200) {
            // nvhttp::pin() returned false: the id had expired out from under
            // us, or the code did not match the request it was sent to.
            emit failed(tr("The machine did not accept the code this device made for it. "
                           "Pairing takes a few seconds and this one ran out of time."),
                        tr("POST /api/pin returned status false."));
            return;
        }

        emit failed(sentenceFor(reply, o[QStringLiteral("error")].toString()),
                    tr("POST /api/pin returned %1.").arg(code == 0 ? reply->errorString()
                                                                   : QString::number(code)));
    });
}
