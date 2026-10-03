#include "machinemodel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

// One RFC 3339 timestamp from Core, as a moment.
//
// The fractional seconds are removed before parsing rather than trusted to a
// parser: Core formats with the `time` crate's rfc3339, which emits whatever
// subsecond precision the column holds — microseconds, from Postgres — and
// how many digits `Qt::ISODate` tolerates is not a thing to discover on a
// buyer's screen. Nothing on a card is measured finer than a second, so the
// digits are dropped rather than rounded.
//
// An absent or unparseable value returns an invalid QDateTime, which the model
// reports as "no observation" rather than as a date. That distinction is the
// whole point of the field.
static QDateTime moment(const QJsonValue& value)
{
    const QString text = value.toString();
    if (text.isEmpty()) {
        return QDateTime();
    }
    static const QRegularExpression fraction(QStringLiteral("\\.\\d+"));
    return QDateTime::fromString(QString(text).remove(fraction), Qt::ISODate);
}

// The grouping, and the one place it is written. Kept as a chain of explicit
// comparisons rather than a set lookup so that the words are visible in a diff
// and a reviewer can hold it beside `MachineCard.qml`'s `statusColour()`,
// which is what the change-budget check does mechanically.
//
// **Every word Core can send is named**, including the three that look like
// faults and are not. Falling through to `Bad` is correct only for Core's own
// "Needs attention": a word this list forgot would arrive as an alarm about a
// machine that is fine.
Machine::Health Machine::health() const
{
    if (status == QStringLiteral("Running") || status == QStringLiteral("Ready")) {
        return Health::Good;
    }
    if (status == QStringLiteral("Deploying") || status == QStringLiteral("Installing")
        || status == QStringLiteral("Starting") || status == QStringLiteral("Restarting")
        || status == QStringLiteral("Stopping")) {
        return Health::Moving;
    }
    if (status == QStringLiteral("Stopped") || status == QStringLiteral("Deleting")) {
        return Health::Resting;
    }
    return Health::Bad;
}

MachineModel::MachineModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int MachineModel::movingCount() const
{
    int n = 0;
    for (const Machine& m : m_machines) {
        if (m.health() == Machine::Health::Moving) { ++n; }
    }
    return n;
}

int MachineModel::unhappyCount() const
{
    int n = 0;
    for (const Machine& m : m_machines) {
        if (m.health() == Machine::Health::Bad) { ++n; }
    }
    return n;
}

int MachineModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_machines.count();
}

QVariant MachineModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_machines.count()) {
        return QVariant();
    }

    const Machine& m = m_machines.at(index.row());
    switch (role) {
    case NameRole:      return m.name;
    case RegionRole:    return m.region;
    case StatusRole:    return m.status;
    case StreamAppRole: return m.streamApp;
    case WebPortRole:   return m.webPort;
    case WorkloadRole:  return m.workload;
    case HasGpuRole:    return m.hasGpu;
    case StreamedRole:  return m.streamed();
    case HostRole:      return m.host;
    case ShortHostRole: return m.shortHost;
    case UserRole:      return m.defaultUser;
    case SummaryRole:   return m.summary;
    case PrivateIpRole: return m.privateIp;
    case LastErrorRole: return m.lastError;
    case OperatingRole: return m.operating;
    case OperationSinceRole:   return m.operationSince;
    case OperationAttemptRole: return m.operationAttempt;
    case WaitingOnRole:        return m.waitingOn;
    case ObservedRole:            return m.observed;
    case ObservedAtRole:          return m.observedAt;
    case ObservationCompleteRole: return m.observationComplete;
    case ProtectedRole:           return m.deletionProtected;
    case AccessRole:    return m.access;
    case ImageRole:     return m.image;
    case VcpusRole:     return m.vcpus;
    case MemoryGibRole: return m.memoryGib;
    case DiskGibRole:   return m.diskGib;
    case GpuModelRole:  return m.gpuModel;
    case GpuCountRole:  return m.gpuCount;
    case PriceRole:     return m.price;
    case HasAppRole:    return m.hasApp;
    case AppNameRole:   return m.appName;
    case DeploymentIdRole: return m.deploymentId;
    case StepNRole:     return m.stepN;
    case StepOfRole:    return m.stepOf;
    case StepLabelRole: return m.stepLabel;
    case AppSinceRole:  return m.appSince;
    case TypicalSecsRole: return m.typicalSecs;
    case AttentionRole: return m.attention;
    case FirstUseRole:  return m.firstUse;
    case LosesRole:     return m.loses;
    // Usable *and* reachable: Core's word is Running or Ready, and it gave
    // the machine a name. A primary action that opens nothing is worse than
    // one that is plainly disabled.
    case ReadyRole:     return usable(m.status) && !m.host.isEmpty();
    default:            return QVariant();
    }
}

QHash<int, QByteArray> MachineModel::roleNames() const
{
    return {
        { NameRole,      "name" },
        { RegionRole,    "region" },
        { StatusRole,    "status" },
        { StreamAppRole, "streamApp" },
        { WebPortRole,   "webPort" },
        { WorkloadRole,  "workload" },
        { HasGpuRole,    "hasGpu" },
        { StreamedRole,  "streamed" },
        { HostRole,      "host" },
        { ShortHostRole, "shortHost" },
        { UserRole,      "user" },
        { SummaryRole,   "summary" },
        { PrivateIpRole, "privateIp" },
        { LastErrorRole, "lastError" },
        { OperatingRole,            "operating" },
        { OperationSinceRole,       "since" },
        { OperationAttemptRole,     "attempt" },
        { WaitingOnRole,            "waitingOn" },
        { ObservedRole,             "observed" },
        { ObservedAtRole,           "observedAt" },
        { ObservationCompleteRole,  "observationComplete" },
        { ProtectedRole,            "protected" },
        { AccessRole,       "access" },
        { ImageRole,        "image" },
        { VcpusRole,        "vcpus" },
        { MemoryGibRole,    "memoryGib" },
        { DiskGibRole,      "diskGib" },
        { GpuModelRole,     "gpuModel" },
        { GpuCountRole,     "gpuCount" },
        { PriceRole,        "price" },
        { HasAppRole,       "hasApp" },
        { AppNameRole,      "appName" },
        { DeploymentIdRole, "deploymentId" },
        { StepNRole,        "stepN" },
        { StepOfRole,       "stepOf" },
        { StepLabelRole,    "stepLabel" },
        { AppSinceRole,     "appSince" },
        { TypicalSecsRole,  "typicalSecs" },
        { AttentionRole,    "attention" },
        { FirstUseRole,     "firstUse" },
        { LosesRole,        "loses" },
        { ReadyRole,     "ready" },
    };
}

// The one place the API's shape is read. Everything above works on Machine.
//
// **The same machines in the same order are updated where they stand; only a
// different list is a reset.** It was a reset every refresh, and a reset
// destroys every card and builds it again: fifteen seconds apart, for ever,
// which repainted each card's picture, dropped whatever the pointer was over,
// and meant a machine going from Starting to Running could never *change* on
// screen — the card that was starting was gone before the one that is running
// arrived. Rows are matched by Core's id, never by name.
// Most specific first: what a person does with the machine outranks what it
// runs on. The Ollama image is a chat; any other web recipe a page; a stream
// a game; then the OS, and a card under a Linux machine shows the card.
QString MachineModel::workloadOf(const QString& image, const QString& osFamily,
                                 bool streamed, int webPort, bool hasGpu)
{
    if (image.contains(QLatin1String("ollama"))) {
        return QStringLiteral("chat");
    }
    if (webPort > 0) {
        return QStringLiteral("web");
    }
    if (streamed) {
        return QStringLiteral("game");
    }
    if (osFamily == QLatin1String("windows")) {
        return QStringLiteral("windows");
    }
    if (hasGpu) {
        return QStringLiteral("gpu");
    }
    return QStringLiteral("linux");
}

void MachineModel::replace(const QJsonArray& machines)
{
    // Remember what each machine was, so a *transition* can be told from a
    // steady state. Announcing "gpu-1 is ready" every fifteen seconds for as
    // long as it stays ready is how people turn notifications off.
    QHash<QString, QString> previous = m_lastStatus;
    QSet<QString> wasWaiting = m_lastWaiting;

    QList<Machine> next;
    next.reserve(machines.size());

    for (const QJsonValue& value : machines) {
        const QJsonObject o = value.toObject();

        Machine m;
        m.id = o["id"].toString();
        m.name = o["name"].toString();
        m.region = o["region"].toString();
        m.status = o["status"].toString();
        m.streamApp = o["stream_app"].toString();
        m.webPort = o["web_port"].toInt();
        m.defaultUser = o["default_user"].toString();
        m.shortHost = o["private_name"].toString();
        m.host = o["private_host"].toString().isEmpty() ? m.shortHost : o["private_host"].toString();
        m.privateIp = o["private_ip"].toString();
        m.lastError = o["last_error"].toString();
        m.deletionProtected = o["protected"].toBool();
        m.access = o["access"].toString();
        m.image = o["image"].toString();
        m.vcpus = o["vcpus"].toInt();
        m.memoryGib = o["memory_mib"].toInt() / 1024;
        m.diskGib = o["disk_gib"].toInt();
        m.price = o["price_per_hour"].toString();

        // The app, when Core sends one. **Its word is the card's** (section 7:
        // `app.status` when present, else `status`); Core makes them one word
        // since step 1, so this only matters against a Core in between.
        const QJsonObject app = o["app"].toObject();
        m.hasApp = !app.isEmpty();
        if (m.hasApp) {
            m.appName = app["name"].toString();
            m.appRecipe = app["recipe_id"].toString();
            m.deploymentId = app["deployment_id"].toString();
            if (!app["status"].toString().isEmpty()) {
                m.status = app["status"].toString();
            }
            const QJsonObject step = app["step"].toObject();
            m.stepN = step["n"].toInt();
            m.stepOf = step["of"].toInt();
            m.stepLabel = step["label"].toString();
            m.appSince = moment(app["since"]);
            m.typicalSecs = app["typical_secs"].toInt();
            m.attention = app["attention"].toString();
            m.firstUse = app["first_use"].toString();
            m.loses = app["loses"].toString();
        }

        // Both objects are omitted entirely when Core has nothing to say, so
        // the test is on the object and not on a field inside it. Core builds
        // `operation` all-or-nothing — id, action and since together or no
        // object — so one presence test is the whole guard.
        const QJsonObject operation = o["operation"].toObject();
        m.operating = !operation.isEmpty();
        m.operationSince = moment(operation["since"]);
        m.operationAttempt = operation["attempt"].toInt(1);
        m.waitingOn = operation["waiting_on"].toString();

        const QJsonObject observation = o["observation"].toObject();
        m.observed = !observation.isEmpty();
        m.observedAt = moment(observation["collected_at"]);
        m.observationComplete = observation["complete"].toBool();

        // Enough to tell two machines apart at a glance, in the units the
        // console uses. GiB rather than MiB: nobody rents 16384 of anything.
        QStringList parts;
        parts << QStringLiteral("%1 vCPU").arg(o["vcpus"].toInt());
        parts << QStringLiteral("%1 GiB").arg(o["memory_mib"].toInt() / 1024);
        const QJsonObject gpu = o["gpu"].toObject();
        m.hasGpu = !gpu.isEmpty();
        // Core's mark when it sends one; the image's guess only for an older Core.
        m.workload = o["mark"].toString().isEmpty()
            ? workloadOf(o["image"].toString(), o["os_family"].toString(), !m.streamApp.isEmpty(), m.webPort, m.hasGpu)
            : o["mark"].toString();
        if (!gpu.isEmpty()) {
            const int count = gpu["count"].toInt(1);
            m.gpuModel = gpu["model"].toString();
            m.gpuCount = count;
            parts << (count > 1 ? QStringLiteral("%1 × %2").arg(count).arg(gpu["model"].toString())
                                : gpu["model"].toString());
        }
        m.summary = parts.join(QStringLiteral(" · "));

        next.append(m);
    }

    bool sameRows = next.size() == m_machines.size();
    for (int i = 0; sameRows && i < next.size(); ++i) {
        sameRows = next.at(i).id == m_machines.at(i).id;
    }
    if (!sameRows) {
        beginResetModel();
    }
    m_machines = next;

    // What to announce, decided here and emitted after the reset, so that a
    // handler which reads this model finds it already consistent.
    struct Change { QString name; QString detail; int kind; };
    enum { Ready, Attention, Waiting };
    QList<Change> changes;

    m_lastStatus.clear();
    m_lastWaiting.clear();
    for (const Machine& m : m_machines) {
        const QString was = previous.value(m.id);
        const bool wasKnown = previous.contains(m.id);
        m_lastStatus.insert(m.id, m.status);

        // Ready is Running with its page answering: one state to a person,
        // so Running → Ready is not announced again.
        const bool running = usable(m.status);
        const bool attention = m.health() == Machine::Health::Bad;

        if (running && !usable(was)) {
            changes.append({ m.name, QString(), Ready });
        }
        // From Running, or from an install under way (an app that did not
        // finish installing is the redesign's "needs attention"), so a machine
        // unhappy since before this client started is not announced, and one
        // unhappy at every poll is announced once.
        else if (attention && wasKnown
                 && (usable(was) || was == QStringLiteral("Installing") || was == QStringLiteral("Deploying"))) {
            changes.append({ m.name, m.attention.isEmpty() ? m.lastError : m.attention, Attention });
        }

        if (!m.waitingOn.isEmpty()) {
            m_lastWaiting.insert(m.id);
            // A machine that is already running and still reports a wait is
            // not something to interrupt anybody for; the wait that matters is
            // the one holding a machine back from existing.
            if (!running && !wasWaiting.contains(m.id)) {
                changes.append({ m.name, m.waitingOn, Waiting });
            }
        }
    }

    if (!sameRows) {
        endResetModel();
    } else if (!m_machines.isEmpty()) {
        emit dataChanged(index(0), index(m_machines.size() - 1));
    }

    // Not on the first load: everything would look new, and a person opening
    // the application does not need to be told about machines they can already
    // see on the screen in front of them.
    if (m_loadedOnce) {
        for (const Change& c : changes) {
            switch (c.kind) {
            case Ready:     emit machineBecameReady(c.name); break;
            case Attention: emit machineNeedsAttention(c.name, c.detail); break;
            case Waiting:   emit machineIsWaiting(c.name, c.detail); break;
            }
        }
    }
    m_loadedOnce = true;
    emit countChanged();
}

void MachineModel::clear()
{
    // **Above the early return, and that placement is the point.** Signing out
    // forgets the machines; it has to forget what they were doing too. Left
    // behind, `m_lastStatus` would make the first refresh after signing back
    // in look like a set of transitions and announce every machine at once —
    // the same defect `m_loadedOnce` exists to prevent, one session later. So
    // the flag goes with them, and it happens even when the list was already
    // empty, which is the case a `return` at the top would have skipped.
    m_lastStatus.clear();
    m_lastWaiting.clear();
    const bool wasLoaded = m_loadedOnce;
    m_loadedOnce = false;

    if (m_machines.isEmpty()) {
        // `loaded` changed even though the list did not.
        if (wasLoaded) {
            emit countChanged();
        }
        return;
    }

    beginResetModel();
    m_machines.clear();
    endResetModel();
    emit countChanged();
}

QVariantMap MachineModel::statusCounts() const
{
    QVariantMap counts;
    for (const Machine& m : m_machines) {
        counts[m.status] = counts.value(m.status).toInt() + 1;
    }
    return counts;
}

QString MachineModel::privateIpAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).privateIp : QString();
}

QString MachineModel::idAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).id : QString();
}

Machine::Health MachineModel::healthAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).health()
                                                  : Machine::Health::Bad;
}

bool MachineModel::protectedAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) && m_machines.at(row).deletionProtected;
}

QString MachineModel::deploymentIdAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).deploymentId : QString();
}

bool MachineModel::usable(const QString& word)
{
    return word == QStringLiteral("Running") || word == QStringLiteral("Ready");
}

QString MachineModel::nameAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).name : QString();
}

QString MachineModel::userAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).defaultUser : QString();
}

int MachineModel::webPortAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).webPort : 0;
}

QString MachineModel::hostAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).host : QString();
}

bool MachineModel::streamedAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) && m_machines.at(row).streamed();
}

QString MachineModel::streamAppAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).streamApp : QString();
}
