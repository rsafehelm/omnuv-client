// Omnuv: the links that name an instance by its id (the Instances redesign,
// D-5, 3 October 2026):
//
//     omnuv://open?instance=<uuid>     OmnuvClient open-instance <uuid>
//     omnuv://stream?instance=<uuid>   OmnuvClient stream-instance <uuid>
//
// A name is reused, a private name moves with a project, and an id is the
// instance's for ever (COMMON_RULES, "Assets are referred to by id"), so the
// web console's Open and Play hand the app an id and the app asks Core where
// that instance answers now. `omnuv://stream?host=` is still accepted by the
// link handlers, for links already out there.

#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

class QObject;
class OmnuvSession;

namespace OmnuvLinkCli
{

// An instance id is a uuid and nothing else: a link is input from any page.
bool validInstanceId(const QString& id);

// What `open-instance` concludes, decided apart from how it found out, so a
// test can hold every branch. `code` is the exit status the link handlers
// branch on: 0 opened, 1 refused, 3 not on the instance's network, which the
// handlers answer by opening the app, whose window offers Join.
struct OpenVerdict
{
    int code = 1;
    QString line;
    QUrl url;
};
OpenVerdict openVerdict(bool signedIn, bool found, bool onNetwork, const QString& name,
                        const QString& host, int webPort, const QString& project,
                        const QString& webUrl = {});

// Exit codes, named once.
enum { Opened = 0, Refused = 1, OffNetwork = 3 };

// The `open-instance` action: headless, one line of stdout, and the browser.
void startOpen(const QStringList& args, QObject* parent);

// The `stream-instance` action's half that runs before any window: where the
// instance answers and what it streams. Blocking, bounded; false with `why`
// when it cannot say, and then nothing is drawn.
bool resolveStream(const QStringList& args, QString* host, QString* app, QString* why);

// The old `stream <host>` form: a host naming one of this account's
// instances becomes that instance's own address; several are refused with
// their ids; a host that is not ours is left as it was.
bool resolveLegacyHost(QString* host, QString* why);

// The one instance `target` names (an id, a host, or a bare live name only
// when unique across the account's projects), with its address and project.
bool locateMachine(OmnuvSession* session, const QString& target, QString* id, QString* host,
                   QString* projectId, QString* why);

} // namespace OmnuvLinkCli
