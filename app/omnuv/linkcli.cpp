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
    QString name, host, app, project;
    int webPort = 0;
};

// Looks for `id` in each of the account's projects in turn, by the session's
// own reads, and calls `done` once: with the instance, or with nothing.
void find(OmnuvSession* session, const QString& id, std::function<void(const Found&)> done)
{
    auto tried = std::make_shared<QSet<QString>>();
    auto over = std::make_shared<bool>(false);
    auto finish = [over, done](const Found& f) {
        if (*over) return;
        *over = true;
        done(f);
    };
    auto look = std::make_shared<std::function<void()>>();
    *look = [session, id, tried, finish, over]() {
        if (*over) return;
        MachineModel* machines = session->machines();
        for (int row = 0; row < machines->rowCount(); ++row) {
            if (machines->idAt(row) != id) continue;
            Found f;
            f.ok = true;
            f.name = machines->nameAt(row);
            f.host = machines->hostAt(row);
            f.app = machines->streamAppAt(row);
            f.webPort = machines->webPortAt(row);
            f.project = session->projectName();
            finish(f);
            return;
        }
        if (!machines->loaded() || session->projectId().isEmpty()) return;
        tried->insert(session->projectId());
        for (const QString& p : session->projectIds()) {
            if (!tried->contains(p)) {
                session->selectProject(p);
                return;
            }
        }
        finish(Found());
    };
    QObject::connect(session->machines(), &MachineModel::countChanged, session, [look]() { (*look)(); });
    QObject::connect(session, &OmnuvSession::projectsChanged, session, [look]() { (*look)(); });
    QTimer::singleShot(kDeadlineMs, session, [finish]() { finish(Found()); });
    (*look)();
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
                                                    int webPort, const QString& project)
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
    if (webPort <= 0 || host.isEmpty()) {
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
    v.url = QUrl(QStringLiteral("http://%1:%2/").arg(host).arg(webPort));
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
    find(session, id, [session](const Found& f) {
        bool onNetwork = false;
        if (f.ok) {
            // Asked now, of the daemon, for the project the instance is in:
            // `find` selected it, which is the scope the membership is held to.
            session->tunnel()->check();
            session->tunnel()->readMembership();
            onNetwork = session->onProjectNetwork();
        }
        const auto v = openVerdict(true, f.ok, onNetwork, f.name, f.host, f.webPort, f.project);
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
    QEventLoop loop;
    Found found;
    find(&session, id, [&](const Found& f) { found = f; loop.quit(); });
    loop.exec();
    if (!found.ok) {
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
