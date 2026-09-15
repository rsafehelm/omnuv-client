// Omnuv: the machines a person rents, as a list QML can show.
//
// The fields are the ones the buyer API already returns. Nothing is derived
// here that Core could decide instead — this model displays, it does not
// choose. That split is what keeps a published client from becoming a second
// place where marketplace behaviour lives.

#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

class QJsonArray;

struct Machine
{
    QString id;
    QString name;
    QString region;
    QString status;
    // The application the machine streams, when it streams one. Empty for an
    // ordinary machine, which is how the two are told apart.
    QString streamApp;
    QString defaultUser;
    QString summary;   // "8 vCPU · 16 GiB · RTX 3090"

    // The name it answers to on the project network, as Core gives it. The
    // only address this application ever uses: a private IP would work today
    // and stop working the moment a machine moves.
    //
    // **Core's, not ours.** This used to be `name.toLower() + ".internal"`,
    // which is the client deciding a naming rule that belongs to the side that
    // allocates the address. It happened to agree with Core, and that is the
    // problem: the day Core prefixes a project, or lowercases differently, or
    // gives a machine a second name, the client would go on confidently
    // building an address nothing answers to. Empty until an address is
    // assigned, which is also when there is nothing to connect to.
    QString host;

    // The address on the project network. Not used to connect — `host` is —
    // but it is the one field that says an address was ever *assigned*, which
    // is what the launch ladder's network step is drawn from.
    QString privateIp;

    // Core's own sentence about what went wrong, when something did. Empty
    // otherwise, and never composed here: a badge reading "Needs attention"
    // with nothing beside it is the same defect as a numeric code, so the card
    // shows this verbatim or says nothing at all.
    QString lastError;

    // What Core has in flight for this machine, when it has anything — the
    // `operation` object of the buyer API's instance view. `operating` is the
    // object's presence rather than a guess from the status word: Core sends
    // all of id/action/since or none of them, so one test covers the lot.
    //
    // `deadlineAt` is deliberately absent. Core's own comment on it says a
    // deadline expiring means re-observe and never conclude, and a client that
    // held it would eventually draw a countdown, which is a client inventing
    // an outcome. Nothing here can use it honestly, so nothing here has it.
    bool operating = false;
    QDateTime operationSince;
    int operationAttempt = 1;
    QString waitingOn;

    // When the *provider* last swept, and whether that sweep finished. Absent
    // when Core sent no observation — which happens when the agent has not
    // reported one or its sweep did not cover machines. Absent is not "just
    // now": a card with no observation says nothing rather than dating a claim
    // nobody made.
    bool observed = false;
    QDateTime observedAt;
    bool observationComplete = false;

    bool streamed() const { return !streamApp.isEmpty(); }

    // **Core's seven words, grouped into the four things a dot can mean.**
    //
    // This is the same grouping `MachineCard.qml`'s `statusColour()` makes,
    // and it has to stay the same grouping: a machine whose card is amber and
    // whose tray row is red is the client telling a person two things about
    // one machine. `.github/workflows/omnuv-change-budget.yml` pins the two
    // against each other and fails the build if they drift.
    //
    // It is a *grouping*, not an interpretation — every word here is one Core
    // sends, and nothing is derived from a field Core did not set. That is the
    // line the tray had to stay on the right side of to be allowed a machine
    // row at all.
    //
    //   Good     Running
    //   Moving   Starting, Restarting, Stopping — an action, not a fault
    //   Resting  Stopped, Deleting
    //   Bad      everything else, which is Core's "Needs attention"
    enum class Health { Good, Moving, Resting, Bad };
    Health health() const;
};

class MachineModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        RegionRole,
        StatusRole,
        StreamAppRole,
        StreamedRole,
        HostRole,
        UserRole,
        SummaryRole,
        PrivateIpRole,
        LastErrorRole,
        OperatingRole,
        OperationSinceRole,
        OperationAttemptRole,
        WaitingOnRole,
        ObservedRole,
        ObservedAtRole,
        ObservationCompleteRole,
        // True when this machine can be connected to right now. A stopped or
        // starting machine is shown, and shown as unreachable, rather than
        // hidden — somebody who cannot find their machine assumes it is lost.
        ReadyRole,
    };

    explicit MachineModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replaces the list with what the API just returned.
    void replace(const QJsonArray& machines);
    void clear();

    // Core's identifier for the machine, which is a join key and never
    // anything a person reads. Deliberately not Q_INVOKABLE and deliberately
    // not a role: this file's own rule is that the id is parsed and not
    // exposed, and the one caller is C++ — OmnuvSession, matching a machine
    // against the deployment it came from, because the buyer API's instance
    // view carries no deployment id and its deployment view carries the
    // instance one.
    QString idAt(int row) const;

    // The grouping for one row. C++-only, like `idAt`: QML has
    // `MachineCard.qml`'s own switch on the same words and does not need a
    // second way to ask, and the tray is C++.
    Machine::Health healthAt(int row) const;

    Q_INVOKABLE QString nameAt(int row) const;
    Q_INVOKABLE QString hostAt(int row) const;
    Q_INVOKABLE QString userAt(int row) const;
    Q_INVOKABLE bool streamedAt(int row) const;
    Q_INVOKABLE QString streamAppAt(int row) const;

    // How many machines are in each of the two states the tray icon cares
    // about, counted from `health()` so that nothing here is a second reading
    // of Core's vocabulary.
    int movingCount() const;
    int unhappyCount() const;

signals:
    // **Three transitions, and only transitions.** This list is short on
    // purpose: a notification for something a person did not ask about, or
    // that they are already looking at, or that repeats every minute for as
    // long as a condition holds, is a notification they turn off — and then
    // the one that mattered is gone too.
    //
    // Each of these fires once, on the refresh where the change happened, and
    // never on the first load, when everything would look new.

    // A machine has finished starting. They asked for it, they have been
    // waiting, and now they can use it.
    void machineBecameReady(const QString& name);

    // A machine that was running is not any more, and not because anybody
    // asked: `Running` → `Needs attention`.
    //
    // **Only from `Running`, and only to `Needs attention`.** `Running` →
    // `Stopping` is somebody pressing Stop, and telling a person what they
    // just did is the definition of noise. `Running` → `Stopped` cannot be
    // told from that at a sixty-second poll, so it is not announced either —
    // the client cannot distinguish a machine that was shut down from one
    // that fell over, and guessing which is exactly the kind of invention
    // this model does not do.
    //
    // `why` is Core's own `last_error`, verbatim, and is empty when Core did
    // not give one.
    void machineNeedsAttention(const QString& name, const QString& why);

    // Something is waiting on capacity, in Core's words. Fires when
    // `operation.waiting_on` appears where there was none — not while it
    // persists, and not when its text merely changes, which would be the same
    // wait announcing itself twice.
    void machineIsWaiting(const QString& name, const QString& waitingOn);

    void countChanged();

private:
    QList<Machine> m_machines;

    // Status per machine id as of the last refresh, so a transition can be
    // told from a steady state. Empty until the first load, which is what
    // stops every machine announcing itself when the application starts.
    QHash<QString, QString> m_lastStatus;

    // Which machines were waiting on something as of the last refresh. The
    // value is not kept, only the fact: a wait whose *reason* changes is the
    // same wait, and re-announcing it would be the repetition this list of
    // signals exists to avoid.
    QSet<QString> m_lastWaiting;

    bool m_loadedOnce = false;
};
