// Omnuv: the signed-in session, and everything this application knows about
// the marketplace.
//
// It does two things. It signs a person in without ever asking them for a
// password — the device authorization grant, where the application shows a
// short code and a browser approves it — and it fetches the machines that
// person rents. Nothing else here talks to Omnuv.
//
// Named omnuvsession rather than session because qmake flattens object file
// names: upstream's streaming/session.cpp already claims session.o, and a
// second one links twice with a wall of duplicate-symbol errors.
//
// What it deliberately does not know: prices, providers, placement, or any
// endpoint an administrator would use. This source is published, so the
// boundary is a security property rather than a matter of taste.

#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

// Included rather than forward-declared: moc needs the full type to expose
// MachineModel* as a Q_PROPERTY.
#include "machinemodel.h"
#include "tunnel.h"

class OmnuvSession : public QObject
{
    Q_OBJECT

    // The deployment this client talks to. Typed once, remembered after.
    Q_PROPERTY(QString coreUrl READ coreUrl WRITE setCoreUrl NOTIFY coreUrlChanged)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY signedInChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    // Shown while a sign-in is waiting for the browser. Empty otherwise.
    Q_PROPERTY(QString userCode READ userCode NOTIFY pendingChanged)
    Q_PROPERTY(QString verificationUri READ verificationUri NOTIFY pendingChanged)

    // One line, in a person's words, about what just happened. The view shows
    // it verbatim; nothing here composes a sentence out of an error code.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

    Q_PROPERTY(MachineModel* machines READ machines CONSTANT)
    Q_PROPERTY(OmnuvTunnel* tunnel READ tunnel CONSTANT)

public:
    explicit OmnuvSession(QObject* parent = nullptr);

    QString coreUrl() const { return m_coreUrl; }
    void setCoreUrl(const QString& url);
    bool signedIn() const { return !m_token.isEmpty(); }
    bool busy() const { return m_busy; }
    QString userCode() const { return m_userCode; }
    QString verificationUri() const { return m_verificationUri; }
    QString status() const { return m_status; }
    MachineModel* machines() const { return m_machines; }
    OmnuvTunnel* tunnel() const { return m_tunnel; }

    // Ask Core for a code, then wait for a browser to approve it. Safe to call
    // again: an unfinished attempt is abandoned first.
    Q_INVOKABLE void signIn();
    Q_INVOKABLE void cancelSignIn();

    // Forgets the token on this device. The token stays valid until it is
    // revoked in the console, which is where taking access back belongs.
    Q_INVOKABLE void signOut();

    // Fetch the machines. Called on start, after signing in, and on a timer.
    Q_INVOKABLE void refresh();

    // Open a terminal on an ordinary machine. Returns false when no terminal
    // could be started, which the view turns into a command to copy rather
    // than into an error — a person with no terminal installed is not stuck,
    // they just have to paste one line.
    Q_INVOKABLE bool openTerminal(const QString& host, const QString& user);

    // Which row of the streaming client's own host list is the machine at this
    // address, or -1.
    //
    // **This exists because the obvious keys are both wrong.** Matching on the
    // host's *name* fails in the window that matters: right after
    // `addNewHostManually` the entry exists but has not been polled, so it has
    // no name yet. Matching on the model's `details` string fails differently
    // and worse — `ComputerModel::data` builds that from `tr("Online")`,
    // `tr("Paired")` and friends, so it is localised prose, and a substring
    // search in it matches 10.200.1.5 against a host at 10.200.1.50.
    //
    // `NvComputer::manualAddress` is the address we passed in, held as
    // structured data and persisted across restarts. It is exact, it is
    // present from the moment the entry is, and it is not translated.
    //
    // The manager arrives as a QObject* because it is a QML singleton owned by
    // the engine; the row is the model's row because `ComputerModel` assigns
    // `m_Computers = getComputers()` verbatim and resets from it on every
    // structural change, so the two orders cannot drift.
    Q_INVOKABLE int hostRowFor(QObject* computerManager, const QString& address) const;



signals:
    void coreUrlChanged();
    void signedInChanged();
    void busyChanged();
    void pendingChanged();
    void statusChanged();

private:
    // Answers the tunnel's needsKey(): asks Core for a one-time enrolment key
    // for this device, then hands it back.
    void fetchDeviceKey();

    void poll();
    void collect();
    void setStatus(const QString& text);
    void setBusy(bool busy);
    void clearPending();
    QNetworkRequest request(const QString& path, bool authenticated) const;

    // Where the token lives. The same file the `omnuv-connect` command writes,
    // so signing in once serves both front ends of the same product.
    //
    // A plain file at mode 0600, not the platform's credential store. That is
    // the right home eventually and it is three different APIs; this is the
    // one thing about the client that should be revisited before release.
    static QString tokenPath();
    void loadToken();
    void saveToken(const QString& token);

    QNetworkAccessManager m_net;
    MachineModel* m_machines;
    OmnuvTunnel* m_tunnel;
    QTimer m_pollTimer;
    QTimer m_refreshTimer;

    QString m_coreUrl;
    QString m_token;
    QString m_deviceCode;
    QString m_userCode;
    QString m_verificationUri;
    QString m_status;
    bool m_busy = false;
};
