#pragma once

#include <QString>
#include <QStringList>

// **omnuv:// is the application's own link** (the operator, 5 October 2026:
// a link from the web console started PowerShell). Windows registers
// `OmnuvClient.exe "%1"` for the scheme and macOS gives it to the client's
// bundle, so a link arrives here as the program's one argument. It is input
// from any web page: every value is held to the same rules the scripts held
// it to (packaging/connect/omnuv-connect.ps1, tests/connect/handle_test.*),
// and a link that breaks one is refused in words, never half-opened.
struct OmnuvDeepLink {
    enum Kind { Refused, Arguments, Ssh, Enrol };
    Kind kind = Refused;
    // Arguments: what the client runs instead, after the program's name
    // (`stream-instance <id>`, `open-instance <id>`, `stream <host> [app]`).
    QStringList arguments;
    // Ssh: the terminal's target, and the machine id its host key is kept by.
    QString host, user, instance;
    // Refused: why, for a person.
    QString why;
};

bool omnuvIsDeepLink(const QString& argument);
// macOS hands a link to the running application as an event, not as an
// argument: each is passed to a new process of this program as its one
// argument, which reads it as Windows' does.
class QCoreApplication;
void omnuvForwardDeepLinks(QCoreApplication* app);
OmnuvDeepLink omnuvParseDeepLink(const QString& url);
