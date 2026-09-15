// Omnuv: the two things this device can find out for itself, with its own
// network stack, without asking anybody to be trustworthy.
//
// The tray's *This device* rows are the only readings in the product that Core
// cannot have and cannot be wrong about in Core's way. They answer one
// question a remote status page structurally cannot:
//
//     Omnuv did not answer. Is that Omnuv, or is it this café's Wi-Fi?
//
// Two readings, and the first exists only to attribute the second. Neither
// carries a credential: `/health` is unauthenticated, so sending the token
// would spend a secret on a check that does not need one, out of a published
// binary.
//
// The third row — the private network — is `OmnuvTunnel`'s, which already
// takes it. This class does not duplicate it.

#pragma once

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

#include "traystate.h"

class OmnuvProbe : public QObject
{
    Q_OBJECT

public:
    explicit OmnuvProbe(QObject* parent = nullptr);

    // Does this device have a route to the internet at all, according to the
    // operating system's own answer rather than to a request we sent
    // somewhere.
    //
    // **Nothing is contacted for this**, and that was the deciding reason.
    // The obvious implementation is a GET against a well-known third party,
    // and in a published client that is a new correspondent on every buyer's
    // machine, every minute, forever, for a fact Windows already computes and
    // Qt already exposes. `Unknown` where no backend is available, which is
    // honest and is why this row cannot raise an alarm on its own — see
    // `traystate.h`.
    omnuv::Reading internet() const;

    // Did Omnuv answer this device. `probed`, not `reported`: it is a local
    // reading about a remote thing, and the distinction is the reason it can
    // be trusted differently from a machine's status.
    omnuv::Reading core() const { return m_core; }

    // When these were last taken. A reading is a claim about a moment, and a
    // stale claim wearing a fresh dot is the failure this whole vocabulary
    // exists to prevent — so the menu renders the age beside every row and
    // this is where it comes from. Invalid until the first one returns, which
    // renders as nothing rather than as "just now".
    QDateTime takenAt() const { return m_takenAt; }

    void setCoreUrl(const QString& url);

    // Take them again.
    void check();

signals:
    void changed();

private:
    void setCore(omnuv::Reading reading);

    // Its own, rather than the session's, so that no path exists from this
    // object to an authenticated request. The header is never set here, and
    // the manager it would have been set on is not reachable from here.
    QNetworkAccessManager m_net;

    QString m_coreUrl;
    omnuv::Reading m_core = omnuv::Reading::Unknown;
    QDateTime m_takenAt;

    // Whether a reachability backend loaded, asked once. False means every
    // `internet()` answers `Unknown` for the life of the process, which is the
    // correct answer and is announced in the log rather than left to look like
    // a healthy quiet reading.
    bool m_haveReachability = false;
};
