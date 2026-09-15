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

    Q_INVOKABLE QString nameAt(int row) const;
    Q_INVOKABLE QString hostAt(int row) const;
    Q_INVOKABLE QString userAt(int row) const;
    Q_INVOKABLE bool streamedAt(int row) const;
    Q_INVOKABLE QString streamAppAt(int row) const;

signals:
    // A machine has finished starting. **The only thing here worth
    // interrupting somebody for**: they asked for it, they have been
    // waiting, and now they can use it. A failed poll is not — it means
    // nothing to a person and it recovers by itself.
    void machineBecameReady(const QString& name);
    void countChanged();

private:
    QList<Machine> m_machines;

    // Status per machine id as of the last refresh, so a transition can be
    // told from a steady state. Empty until the first load, which is what
    // stops every machine announcing itself when the application starts.
    QHash<QString, QString> m_lastStatus;
    bool m_loadedOnce = false;
};
