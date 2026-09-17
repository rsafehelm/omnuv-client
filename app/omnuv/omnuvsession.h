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
#include "estate.h"
#include "appearance.h"
#include "autostart.h"
#include "tunnel.h"

class OmnuvPairing;

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

    // **The projects this person is a member of, and the one in view.**
    //
    // A buyer may belong to several, and everything else follows from which is
    // chosen: which machines are listed, which network a device joins, which
    // endpoints are theirs. Until this existed the client asked for machines
    // and networks with no project at all and took whatever came back first —
    // unambiguous only while a tenant has exactly one, which is luck rather
    // than design.
    //
    // Names and ids in step, for a view that shows one and acts on the other.
    Q_PROPERTY(QStringList projectNames READ projectNames NOTIFY projectsChanged)
    Q_PROPERTY(QStringList projectIds READ projectIds NOTIFY projectsChanged)
    Q_PROPERTY(QString projectId READ projectId WRITE selectProject NOTIFY projectsChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectsChanged)

    // `/v1/me` has answered and named no project at all — an organization
    // whose last project was torn down. Nothing project-scoped can be asked
    // then, so the window says so instead of waiting on reads never made.
    Q_PROPERTY(bool noProject READ noProject NOTIFY projectsChanged)

    Q_PROPERTY(MachineModel* machines READ machines CONSTANT)

    // Everything else the window shows about the project in view: see
    // `estate.h`. Read only while the window is open, because nothing else
    // draws it.
    Q_PROPERTY(OmnuvEstate* estate READ estate CONSTANT)
    Q_PROPERTY(OmnuvTunnel* tunnel READ tunnel CONSTANT)
    Q_PROPERTY(OmnuvAutostart* autostart READ autostart CONSTANT)

    // What the operating system asked every application to look like and to
    // move like. Read-only here and everywhere: see `appearance.h`.
    Q_PROPERTY(OmnuvAppearance* appearance READ appearance CONSTANT)

public:
    explicit OmnuvSession(QObject* parent = nullptr);

    QString coreUrl() const { return m_coreUrl; }
    void setCoreUrl(const QString& url);
    bool signedIn() const { return !m_token.isEmpty(); }
    bool busy() const { return m_busy; }
    QString userCode() const { return m_userCode; }
    QString verificationUri() const { return m_verificationUri; }
    QString status() const { return m_status; }
    QStringList projectNames() const { return m_projectNames; }
    QStringList projectIds() const { return m_projectIds; }
    // **Whose token this is.** `/v1/me` has always returned it and the client
    // has always thrown it away, which is how a rig signed in as one account
    // spent an afternoon reporting no machines for another's project. Empty
    // until the identity has arrived.
    QString accountEmail() const { return m_accountEmail; }
    QString projectId() const { return m_projectId; }
    bool noProject() const { return m_identityKnown && m_projectIds.isEmpty(); }
    QString projectName() const
    {
        const int at = m_projectIds.indexOf(m_projectId);
        return at >= 0 ? m_projectNames.at(at) : QString();
    }

    // Chosen by a person, and remembered. Re-reads what belongs to it rather
    // than leaving the previous project's machines on screen.
    Q_INVOKABLE void selectProject(const QString& id);

    MachineModel* machines() const { return m_machines; }
    OmnuvEstate* estate() const { return m_estate; }
    OmnuvTunnel* tunnel() const { return m_tunnel; }
    OmnuvAutostart* autostart() const { return m_autostart; }
    OmnuvAppearance* appearance() const { return m_appearance; }

    // Whether this launch should go straight to the tray instead of opening a
    // window, and it records that a launch happened.
    //
    // **Never on the first run.** A program that installs itself, starts, and
    // shows nothing is indistinguishable from malware — to a person and to a
    // scanner. The first launch opens and signs in; only later ones are
    // allowed to be quiet.
    //
    // And only when autostart is on, which is the person having asked for a
    // background program. With it off, every launch shows a window, because
    // somebody double-clicking an icon and getting nothing at all is the same
    // bad experience by a different route.
    //
    // (The honest alternative is to tag the autostart entry with a flag and
    // read it here. Upstream's parser calls `handleUnknownOptions()`, which
    // terminates the process on any argument it does not know, so that costs
    // an edit to a file outside the change budget. Not worth it for this.)
    Q_INVOKABLE bool shouldStartHidden();

    // Called by the window when it is shown or hidden, so the poll rate
    // follows whether anybody is actually looking.
    Q_INVOKABLE void setWindowVisible(bool visible);

    // Ask Core for a code, then wait for a browser to approve it. Safe to call
    // again: an unfinished attempt is abandoned first.
    Q_INVOKABLE void signIn();
    Q_INVOKABLE void cancelSignIn();

    // Forgets the token on this device. The token stays valid until it is
    // revoked in the console, which is where taking access back belongs.
    Q_INVOKABLE void signOut();

    // Fetch the machines. Called on start, after signing in, and on a timer.
    // `everything` also re-reads the parts of the estate that change slowly,
    // which the timer reads only every few ticks: a person who pressed
    // Refresh, or just opened the window, means all of it.
    Q_INVOKABLE void refresh(bool everything = false);

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

    // **Pairing, without anybody typing four digits into anything.**
    //
    // Call this straight after ComputerModel::pairComputer() for the same
    // machine, with the same PIN. It finds the deployment the machine came
    // from, collects the one-time login that machine minted for its own
    // streaming host, and hands the PIN over with it. The view shows its
    // dialog only if this fails.
    //
    // The order matters and is the machine's rather than ours: the identifier
    // a PIN is addressed to does not exist until the client's own pairing
    // request is waiting. See `pairing.cpp`.
    //
    // Exactly one of pairingSucceeded() or pairingFailed() follows, always.
    Q_INVOKABLE void deliverPin(int row, const QString& pin);



signals:
    void projectsChanged();
    void coreUrlChanged();
    void signedInChanged();
    void busyChanged();
    void pendingChanged();
    void statusChanged();

    // The machine took the code. Whether the pairing then completed is
    // ComputerModel::pairingCompleted's answer; this only says the delivery
    // arrived.
    void pairingSucceeded();

    // `why` is one sentence for the person, already in their words. `detail`
    // is the machine-shaped remainder the view folds away behind a disclosure
    // — a status, an API message — and never shows on its own.
    void pairingFailed(const QString& why, const QString& detail);

private:
    // Answers the tunnel's needsKey(): asks Core for a one-time enrolment key
    // for this device, then hands it back.
    void fetchDeviceKey();

    // The second half of deliverPin(): collect the one-time streaming login
    // for a deployment and spend it. Separate because `waiting` is a normal
    // answer that is asked again, not a failure.
    void collectStreamLogin(int row, const QString& deploymentId,
                            const QString& pin, int triesLeft);

    void poll();
    void collect();
    void setStatus(const QString& text);
    void setBusy(bool busy);
    void clearPending();
    QNetworkRequest request(const QString& path, bool authenticated) const;

    // Where the token lives. The same file the `omnuv-connect` command writes,
    // so signing in once serves both front ends of the same product.
    //
    // Where it actually lives is `credentials.h`: Credential Manager on
    // Windows, a 0600 file where that is not implemented. This used to be a
    // plain file everywhere, described here as "the one thing about the client
    // that should be revisited before release" — residency is what made that
    // urgent, because a widget holds a token across reboots rather than for
    // the minutes an application is open.
    void loadToken();
    void saveToken(const QString& token);

    QNetworkAccessManager m_net;
    MachineModel* m_machines;
    OmnuvEstate* m_estate;
    bool m_windowVisible = true;
    OmnuvTunnel* m_tunnel;
    OmnuvAutostart* m_autostart;
    OmnuvAppearance* m_appearance;
    OmnuvPairing* m_pairing;
    // Held rather than re-fetched: every request that names a project reads
    // `m_projectId`, and a list that arrives once per sign-in does not need to
    // arrive again per call.
    void fetchIdentity();
    QString projectQuery() const;

    QStringList m_projectNames;
    QStringList m_projectIds;
    QString m_accountEmail;
    bool m_identityKnown = false;
    bool m_identityPending = false;

    // Revoked in the console, or expired: forget the token and say so.
    void accessTakenBack();
    QString m_projectId;

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
