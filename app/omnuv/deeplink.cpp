#include "deeplink.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFileOpenEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>

namespace {

OmnuvDeepLink refused(const QString& why)
{
    OmnuvDeepLink link;
    link.why = why;
    return link;
}

// The scripts' rules, one for one (Assert-Link* in omnuv-connect.ps1).
bool hostIsSafe(const QString& host)
{
    static const QRegularExpression name(QStringLiteral(
        "\\A[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\\z"));
    return host.size() <= 253 && name.match(host).hasMatch();
}

bool userIsSafe(const QString& user)
{
    static const QRegularExpression name(QStringLiteral("\\A[A-Za-z_][A-Za-z0-9_.-]{0,31}\\z"));
    return user.isEmpty() || name.match(user).hasMatch();
}

bool instanceIsSafe(const QString& id)
{
    static const QRegularExpression uuid(QStringLiteral(
        "\\A[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\z"));
    return uuid.match(id).hasMatch();
}

bool appIsSafe(const QString& app)
{
    static const QRegularExpression bad(QStringLiteral("\\A-|[\"\\x00-\\x1f\\x7f]"));
    return app.isEmpty() || (app.size() <= 128 && !bad.match(app).hasMatch());
}

} // namespace

bool omnuvIsDeepLink(const QString& argument)
{
    return argument.startsWith(QLatin1String("omnuv:"), Qt::CaseInsensitive);
}

OmnuvDeepLink omnuvParseDeepLink(const QString& url)
{
    if (!url.startsWith(QLatin1String("omnuv://"), Qt::CaseInsensitive)) {
        return refused(QStringLiteral("not an omnuv link: %1").arg(url));
    }
    // The action is everything before the query, as the scripts read it: a
    // browser may hand over `omnuv://open/?…` as well as `omnuv://open?…`.
    const QString rest = url.mid(8);
    const int mark = rest.indexOf(QLatin1Char('?'));
    QString action = (mark < 0 ? rest : rest.left(mark)).toLower();
    while (action.endsWith(QLatin1Char('/'))) action.chop(1);
    // A '+' is a space, as the scripts and every form encoder have it; made
    // one before decoding, so an encoded %2B stays a plus.
    QString raw = mark < 0 ? QString() : rest.mid(mark + 1);
    raw.replace(QLatin1Char('+'), QLatin1String("%20"));
    QUrlQuery query(raw);
    auto value = [&query](const char* key) {
        return query.queryItemValue(QString::fromLatin1(key), QUrl::FullyDecoded);
    };

    if (action == QLatin1String("enrol") || action == QLatin1String("enroll") || action == QLatin1String("join")) {
        if (!value("key").isEmpty()) {
            return refused(QStringLiteral("a link can no longer carry a network key. Open Omnuv, sign in and use Join this device."));
        }
        OmnuvDeepLink link;
        link.kind = OmnuvDeepLink::Enrol;
        return link;
    }
    if (action == QLatin1String("stream")) {
        const QString instance = value("instance");
        if (!instance.isEmpty()) {
            if (!instanceIsSafe(instance)) return refused(QStringLiteral("that link names an instance this will not open: %1").arg(instance));
            OmnuvDeepLink link;
            link.kind = OmnuvDeepLink::Arguments;
            link.arguments = {QStringLiteral("stream-instance"), instance};
            return link;
        }
        const QString host = value("host"), app = value("app");
        if (host.isEmpty()) return refused(QStringLiteral("that link names no machine"));
        if (!hostIsSafe(host)) return refused(QStringLiteral("that link names a machine this will not open: %1").arg(host));
        if (!appIsSafe(app)) return refused(QStringLiteral("that link names an application this will not open: %1").arg(app));
        OmnuvDeepLink link;
        link.kind = OmnuvDeepLink::Arguments;
        link.arguments = {QStringLiteral("stream"), host};
        if (!app.isEmpty()) link.arguments << app;
        return link;
    }
    if (action == QLatin1String("open")) {
        const QString instance = value("instance");
        if (instance.isEmpty()) return refused(QStringLiteral("that link names no instance"));
        if (!instanceIsSafe(instance)) return refused(QStringLiteral("that link names an instance this will not open: %1").arg(instance));
        OmnuvDeepLink link;
        link.kind = OmnuvDeepLink::Arguments;
        link.arguments = {QStringLiteral("open-instance"), instance};
        return link;
    }
    if (action == QLatin1String("ssh")) {
        const QString host = value("host"), user = value("user"), instance = value("instance");
        if (host.isEmpty()) return refused(QStringLiteral("that link names no machine"));
        if (!hostIsSafe(host)) return refused(QStringLiteral("that link names a machine this will not open: %1").arg(host));
        if (!userIsSafe(user)) return refused(QStringLiteral("that link names a user this will not open: %1").arg(user));
        if (instance.isEmpty()) return refused(QStringLiteral("that link names no instance, and ssh needs the machine's id for its host key"));
        if (!instanceIsSafe(instance)) return refused(QStringLiteral("that link names an instance this will not open: %1").arg(instance));
        OmnuvDeepLink link;
        link.kind = OmnuvDeepLink::Ssh;
        link.host = host;
        link.user = user;
        link.instance = instance;
        return link;
    }
    return refused(QStringLiteral("unknown link: omnuv://%1").arg(action));
}

namespace {
class Forwarder : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::FileOpen) {
            const QString url = static_cast<QFileOpenEvent*>(event)->url().toString();
            if (omnuvIsDeepLink(url)) {
                qInfo().noquote() << "omnuv: link handed to a new process:" << url.left(200);
                QProcess::startDetached(QCoreApplication::applicationFilePath(), {url});
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }
};
} // namespace

void omnuvForwardDeepLinks(QCoreApplication* app)
{
    app->installEventFilter(new Forwarder(app));
}
