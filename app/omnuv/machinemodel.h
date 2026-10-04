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
#include <QVariantMap>

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
    // A web recipe's page port on the machine's private name (Core's
    // `web_port`, 2 October 2026): Play opens a browser there instead.
    int webPort = 0;
    // **What the machine is for, as the card draws it** (the operator, 3
    // October 2026: "a stylized logo representing the workload"). One of
    // chat, web, game, windows, gpu, linux; derived in `workloadOf()` from
    // what Core already sends, so no new field crosses the API.
    QString workload;
    bool hasGpu = false;
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
    // **What the card shows**, Core's `private_name` (`ollama-53746c3e.internal`):
    // short enough to read. `host` above is what everything connects to, and is
    // Core's `private_host` when it sends one: `<machine uuid>.<project uuid>.
    // <domain>`, unique to this machine for ever (3 October 2026), so nothing
    // on this device can reach a machine made before under the same name.
    QString shortHost;

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

    // Protected against deletion: Core's `protected` (migration 0195, D26).
    // Core refuses its delete unless the owner confirms, so the view's
    // confirmation says so, and only then asks Core to clear it. An older
    // Core sends nothing, which is not protected.
    bool deletionProtected = false;

    // **What the card draws, from Core** (the Instances redesign, 3 October
    // 2026; docs/plans/recipes-in-instances.md section 4). `mark` and `access`
    // are Core's when it sends them; an older Core sends neither and the mark
    // falls back to `workloadOf()`.
    QString access;
    QString image;
    int vcpus = 0;
    int memoryGib = 0;
    int diskGib = 0;
    QString gpuModel;
    int gpuCount = 0;
    QString price;   // Core's `price_per_hour`, an estimate while nothing charges

    // The app on it, when it was deployed from a recipe (Core's `app`). The
    // deployment's id is what pairing claims a stream login with, so the card
    // never reads `/v1/deployments` to find it again.
    bool hasApp = false;
    QString appName;
    QString appRecipe;
    QString deploymentId;
    int stepN = 0;
    int stepOf = 0;
    QString stepLabel;
    QDateTime appSince;
    int typicalSecs = 0;
    QString attention;
    QString firstUse;
    QString loses;

    bool streamed() const { return !streamApp.isEmpty(); }

    // **Core's seven words, grouped into the four things a dot can mean.**
    //
    // This is the same grouping `MachineCard.qml`'s `statusColour()` makes,
    // and it has to stay the same grouping: a machine whose card is amber and
    // whose tray row is red is the client telling a person two things about
    // one machine. `app/omnuv/test/checks.sh` pins the two
    // against each other and fails if they drift.
    //
    // It is a *grouping*, not an interpretation — every word here is one Core
    // sends, and nothing is derived from a field Core did not set. That is the
    // line the tray had to stay on the right side of to be allowed a machine
    // row at all.
    //
    //   Good     Running, Ready
    //   Moving   Deploying, Installing, Starting, Restarting, Stopping —
    //            an action, not a fault
    //   Resting  Stopped, Deleting
    //   Bad      everything else, which is Core's "Needs attention"
    enum class Health { Good, Moving, Resting, Bad };
    Health health() const;
};

class MachineModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

    // How many machines carry each of Core's status words — the estate's
    // pulse row. Counted, not interpreted: the keys are Core's words.
    Q_PROPERTY(QVariantMap statusCounts READ statusCounts NOTIFY countChanged)

    // Whether the list has been read at all since signing in. A list nobody
    // has read yet is not an empty list, and the pulse must not draw it as
    // one.
    Q_PROPERTY(bool loaded READ loaded NOTIFY countChanged)

public:
    // The card's workload mark, from the image, the OS, and what the machine
    // serves. Static, so a test can hold every case to a word.
    static QString workloadOf(const QString& image, const QString& osFamily,
                              bool streamed, int webPort, bool hasGpu);

    // **Which mark the tile draws** (the operator, 4 October 2026: a Windows
    // machine is marked as Windows): Core's mark and the image's OS, the OS
    // first only when it is Windows; Linux as before; a plain display when
    // neither says. The web's `markFor` (console-shared/src/marks.ts), row
    // for row. The deploy dialog's tiles go through it too.
    static QString markFor(const QString& mark, const QString& osFamily);

    enum Role {
        NameRole = Qt::UserRole + 1,
        RegionRole,
        StatusRole,
        StreamAppRole,
        WebPortRole,
        WorkloadRole,
        HasGpuRole,
        StreamedRole,
        HostRole,
        ShortHostRole,
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
        // `protected` to QML, as Core names it.
        ProtectedRole,
        AccessRole,
        ImageRole,
        VcpusRole,
        MemoryGibRole,
        DiskGibRole,
        GpuModelRole,
        GpuCountRole,
        PriceRole,
        HasAppRole,
        AppNameRole,
        DeploymentIdRole,
        StepNRole,
        StepOfRole,
        StepLabelRole,
        AppSinceRole,
        TypicalSecsRole,
        AttentionRole,
        FirstUseRole,
        LosesRole,
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
    Q_INVOKABLE QString idAt(int row) const;
    // Core's overlay address for the machine at this row, or "".
    QString privateIpAt(int row) const;

    // The grouping for one row. C++-only, like `idAt`: QML has
    // `MachineCard.qml`'s own switch on the same words and does not need a
    // second way to ask, and the tray is C++.
    Machine::Health healthAt(int row) const;

    // Whether the machine in `row` is protected now. C++-only: OmnuvSession
    // asks it when a confirmed delete is sent; QML reads the role.
    bool protectedAt(int row) const;

    // The app's deployment on this row, or "": what pairing claims a stream
    // login with. C++-only, like `idAt`.
    QString deploymentIdAt(int row) const;

    // Whether Core's word means the machine is usable now: Running or Ready.
    static bool usable(const QString& word);

    // **Which rows `target` names, by id first** (the assets-by-id audit, 3
    // October 2026). A whole instance id names its row; a host equal to a
    // row's host (Core's name by id, any case) names that row; Core's older
    // private name names the live rows that carry it; otherwise a bare name
    // (the label before the first dot) names every live row with that name,
    // a machine being deleted excluded. The caller decides what more
    // than one means: never "the first".
    QList<int> rowsFor(const QString& target) const;
    // A whole uuid, as Core writes an instance's id.
    static bool isInstanceId(const QString& s);

    Q_INVOKABLE QString nameAt(int row) const;
    Q_INVOKABLE QString hostAt(int row) const;
    Q_INVOKABLE QString userAt(int row) const;
    Q_INVOKABLE bool streamedAt(int row) const;
    Q_INVOKABLE QString streamAppAt(int row) const;
    Q_INVOKABLE int webPortAt(int row) const;

    // How many machines are in each of the two states the tray icon cares
    // about, counted from `health()` so that nothing here is a second reading
    // of Core's vocabulary.
    int movingCount() const;
    QVariantMap statusCounts() const;
    bool loaded() const { return m_loadedOnce; }
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
