#include "linkcli.h"
#include "machinemodel.h"
#include "omnuvsession.h"
#include "tunnel.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QEventLoop>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>

#include <stdio.h>

// The stdout contract, as `signin`, `enrol` and `pair-machine` keep it: this
// file is the only writer, one verdict line, and Qt's logging goes elsewhere.
namespace {

void emitLine(const QString& line)
{
    fputs(qPrintable(line + QLatin1Char('\n')), stdout);
    fflush(stdout);
}

// Finding an instance reads its project's list, and an account may hold
// several; each answers in about a second. A run that cannot find it says so
// rather than leave a browser click hanging.
const int kDeadlineMs = 30 * 1000;

struct Found
{
    bool ok = false;
    QString id, name, host, app, project, projectId;
    int webPort = 0;
    QString webUrl;
};

// **Every machine `target` names, in each of the account's projects in
// turn** (the assets-by-id audit, 3 October 2026), by the session's own
// reads: `MachineModel::rowsFor`, so an id, then a host, then a bare live
// name. A whole id stops at its machine; anything else visits every project,
// so a name shared across projects is seen as shared. Calls `done` once.
void scan(OmnuvSession* session, const QString& target, std::function<void(const QList<Found>&)> done)
{
    const bool byId = MachineModel::isInstanceId(target);
    auto tried = std::make_shared<QSet<QString>>();
    auto hits = std::make_shared<QList<Found>>();
    auto over = std::make_shared<bool>(false);
    auto finish = [over, done, hits]() {
        if (*over) return;
        *over = true;
        done(*hits);
    };
    auto look = std::make_shared<std::function<void()>>();
    *look = [session, target, byId, tried, hits, finish, over]() {
        if (*over) return;
        MachineModel* machines = session->machines();
        const QString project = session->projectId();
        if (!machines->loaded() || project.isEmpty() || tried->contains(project)) return;
        tried->insert(project);
        for (int row : machines->rowsFor(target)) {
            Found f;
            f.ok = true;
            f.id = machines->idAt(row);
            f.name = machines->nameAt(row);
            f.host = machines->hostAt(row);
            f.app = machines->streamAppAt(row);
            f.webPort = machines->webPortAt(row);
            f.webUrl = machines->webUrlAt(row);
            f.project = session->projectName();
            f.projectId = project;
            hits->append(f);
        }
        if (byId && !hits->isEmpty()) {
            finish();
            return;
        }
        for (const QString& p : session->projectIds()) {
            if (!tried->contains(p)) {
                session->selectProject(p);
                return;
            }
        }
        finish();
    };
    QObject::connect(session->machines(), &MachineModel::countChanged, session, [look]() { (*look)(); });
    QObject::connect(session, &OmnuvSession::projectsChanged, session, [look]() { (*look)(); });
    QTimer::singleShot(kDeadlineMs, session, [finish]() { finish(); });
    (*look)();
}

// The one machine `target` names, or why there is none: several are refused
// with their ids, never resolved to the first.
bool locate(OmnuvSession* session, const QString& target, Found* found, QString* why)
{
    QEventLoop loop;
    QList<Found> hits;
    scan(session, target, [&](const QList<Found>& h) { hits = h; loop.quit(); });
    loop.exec();
    if (hits.size() == 1) {
        *found = hits.first();
        return true;
    }
    if (hits.isEmpty()) {
        *why = QStringLiteral("no instance of yours is %1").arg(target);
        return false;
    }
    QStringList ids;
    for (const Found& f : hits) ids << f.id;
    *why = QStringLiteral("%1 instances answer to %2 (%3); name one by its id")
               .arg(hits.size()).arg(target, ids.join(QStringLiteral(", ")));
    return false;
}

} // namespace

bool OmnuvLinkCli::validInstanceId(const QString& id)
{
    static const QRegularExpression uuid(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return uuid.match(id).hasMatch();
}

OmnuvLinkCli::OpenVerdict OmnuvLinkCli::openVerdict(bool signedIn, bool found, bool onNetwork,
                                                    const QString& name, const QString& host,
                                                    int webPort, const QString& project,
                                                    const QString& webUrl)
{
    OpenVerdict v;
    if (!signedIn) {
        v.line = QStringLiteral("state=refused  reason=sign in to Omnuv in the app, then open it again");
        return v;
    }
    if (!found) {
        v.line = QStringLiteral("state=refused  reason=no instance of yours has that id");
        return v;
    }
    if (webUrl.isEmpty() && (webPort <= 0 || host.isEmpty())) {
        v.line = QStringLiteral("state=refused  reason=%1 has no page to open yet").arg(name);
        return v;
    }
    // **Refused off the network, never opened anyway** (D-5): the page is on
    // the project's private network and nowhere else, so a browser sent there
    // from a device that is not on it shows a timeout and blames the instance.
    if (!onNetwork) {
        v.code = OffNetwork;
        v.line = QStringLiteral("state=off-network  reason=this device is not on %1's private network; "
                                "join it in the app, then open %2 again").arg(project, name);
        return v;
    }
    v.code = Opened;
    // Core's HTTPS address once the machine holds its certificate (0236),
    // else the page's own port, as before.
    v.url = !webUrl.isEmpty() ? QUrl(webUrl) : QUrl(QStringLiteral("http://%1:%2/").arg(host).arg(webPort));
    v.line = QStringLiteral("state=opened  url=%1").arg(v.url.toString());
    return v;
}

void OmnuvLinkCli::startOpen(const QStringList& args, QObject* parent)
{
    const QString id = args.size() > 2 ? args.at(2) : QString();
    if (!validInstanceId(id)) {
        emitLine(QStringLiteral("state=refused  reason=that link names no instance"));
        ::exit(Refused);
    }
    auto* session = new OmnuvSession(parent);
    if (!session->signedIn()) {
        const auto v = openVerdict(false, false, false, {}, {}, 0, {});
        emitLine(v.line);
        ::exit(v.code);
    }
    scan(session, id, [session](const QList<Found>& hits) {
        const Found f = hits.value(0);
        bool onNetwork = false;
        if (f.ok) {
            // Asked now, of the daemon, for the project the instance is in:
            // `scan` selected it, which is the scope the membership is held to.
            session->tunnel()->check();
            session->tunnel()->readMembership();
            onNetwork = session->onProjectNetwork();
        }
        const auto v = openVerdict(true, f.ok, onNetwork, f.name, f.host, f.webPort, f.project, f.webUrl);
        if (v.code == Opened && !QDesktopServices::openUrl(v.url)) {
            emitLine(QStringLiteral("state=refused  reason=no browser could be opened; open %1 yourself")
                         .arg(v.url.toString()));
            QMetaObject::invokeMethod(QCoreApplication::instance(), []() { QCoreApplication::exit(Refused); },
                                      Qt::QueuedConnection);
            return;
        }
        emitLine(v.line);
        const int code = v.code;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [code]() { QCoreApplication::exit(code); },
                                  Qt::QueuedConnection);
    });
}

bool OmnuvLinkCli::resolveStream(const QStringList& args, QString* host, QString* app, QString* why)
{
    const QString id = args.size() > 2 ? args.at(2) : QString();
    if (!validInstanceId(id)) {
        *why = QStringLiteral("that link names no instance");
        return false;
    }
    OmnuvSession session;
    if (!session.signedIn()) {
        *why = QStringLiteral("sign in to Omnuv in the app, then press Play again");
        return false;
    }
    Found found;
    if (!locate(&session, id, &found, why)) {
        *why = QStringLiteral("no instance of yours has that id");
        return false;
    }
    if (found.host.isEmpty()) {
        *why = QStringLiteral("%1 has no address on your network yet").arg(found.name);
        return false;
    }
    *host = found.host;
    *app = found.app;
    return true;
}

bool OmnuvLinkCli::resolveLegacyHost(QString* host, QString* why)
{
    // The old `stream <host>` form (`omnuv-connect --stream`, `omnuv://stream?
    // host=`): upstream's seeker matches a saved host by name, and a name is
    // reused the moment its machine is deleted and made again. When the host
    // names one of this account's instances it becomes that instance's own
    // address, which only it answers to; when it names several, it is
    // refused with their ids; when it names none, it is not ours and goes on
    // as upstream's, untouched. Signed out, nothing can be asked.
    OmnuvSession session;
    if (!session.signedIn() || host->trimmed().isEmpty()) {
        return true;
    }
    Found found;
    QString whyNot;
    if (locate(&session, *host, &found, &whyNot)) {
        if (found.host.isEmpty()) {
            *why = QStringLiteral("%1 has no address on your network yet").arg(found.name);
            return false;
        }
        *host = found.host;
        return true;
    }
    if (whyNot.startsWith(QStringLiteral("no instance"))) {
        return true;
    }
    *why = whyNot;
    return false;
}

bool OmnuvLinkCli::locateMachine(OmnuvSession* session, const QString& target, QString* id, QString* host,
                                 QString* projectId, QString* why)
{
    Found found;
    if (!locate(session, target, &found, why)) {
        return false;
    }
    *id = found.id;
    *host = found.host;
    *projectId = found.projectId;
    return true;
}

