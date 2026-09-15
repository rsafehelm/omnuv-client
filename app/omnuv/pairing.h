// Omnuv: the half of pairing that talks to the machine.
//
// Moonlight's pairing has always ended with a person reading four digits off
// one screen and typing them into another. That is the last step of renting a
// machine that asks the buyer to be an administrator of it, and it exists only
// because the client has no way to prove to the machine that it is entitled to
// pair. It now has one: the machine mints a single-use login for its own
// streaming host, reports it to Core through the ordinary status channel, and
// the owner's client collects it once and spends it here.
//
// So this class delivers a PIN the person never sees, and everything it knows
// about the machine's API was read from the machine's own source — see the
// comments on each call for the file, the tag and the line.
//
// **It holds no credential.** The login arrives as an argument, travels in a
// lambda capture to the one request that spends it, and is never a member,
// never a setting, never a log line. `OmnuvSession` fetches it; nothing
// stores it.

#pragma once

#include <QList>
#include <QObject>
#include <QSslError>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

class OmnuvPairing : public QObject
{
    Q_OBJECT

public:
    // Borrows the session's network access manager rather than making a
    // second one: one manager means one connection pool and one proxy policy,
    // and the per-request TLS decision below is per-request, so there is
    // nothing to isolate.
    explicit OmnuvPairing(QNetworkAccessManager* net, QObject* parent = nullptr);

    // Hand `pin` to the streaming host on `host`, as the client called
    // `clientName`, using the one-time web login `user`/`password`.
    //
    // Call it only *after* ComputerModel::pairComputer() has been called for
    // the same machine: the identifier this needs does not exist until the
    // client's own pairing request has arrived and is waiting. That order is
    // the machine's, not ours — see the comment on findPendingPairing().
    //
    // `fromAddress` is this device's address on the project network, used to
    // pick our own pairing request out of the list when more than one is
    // waiting. Empty is tolerated; see findPendingPairing().
    void deliver(const QString& host,
                 const QString& pin,
                 const QString& clientName,
                 const QString& fromAddress,
                 const QString& user,
                 const QString& password);

    // Give up before the machine was ever asked — for the cases OmnuvSession
    // decides, so that every ending of a pairing attempt arrives on one
    // signal. The same shape as OmnuvTunnel::giveUp().
    void giveUp(const QString& why, const QString& detail = QString());

signals:
    // The machine took the PIN. Whether the pairing itself then completed is
    // ComputerModel::pairingCompleted's answer, not ours.
    void delivered();

    // `why` is a sentence for the person. `detail` is the machine-shaped
    // remainder — a status code, an API message — which the view folds away
    // behind a disclosure and never shows on its own.
    void failed(const QString& why, const QString& detail);

private:
    void findPendingPairing(const QString& host,
                            const QString& pin,
                            const QString& clientName,
                            const QString& fromAddress,
                            const QString& user,
                            const QString& password,
                            int triesLeft);

    void sendPin(const QString& host,
                 const QString& pairingId,
                 const QString& pin,
                 const QString& clientName,
                 const QString& user,
                 const QString& password);

    // A request to the machine's own web API, carrying the one-time login.
    QNetworkRequest webRequest(const QString& host,
                               const QString& user,
                               const QString& password) const;

    // Accept the streaming host's self-signed certificate, and only the
    // errors that being self-signed actually causes. See the implementation
    // for what this rests on, which is the overlay and nothing else.
    static void acceptSelfSigned(QNetworkReply* reply, const QList<QSslError>& errors);

    // One sentence for whatever the machine's web API said, in the words a
    // buyer uses. Never a number: the number goes in `detail`.
    static QString sentenceFor(QNetworkReply* reply, const QString& apiError);

    QNetworkAccessManager* m_net;
};
