#pragma once

#include <QStringList>

class QObject;

// Omnuv: sign this device in from a shell, with the code on stdout.
//
// `Moonlight.exe signin` runs the device authorization grant this application
// already runs from its window, and writes what a script needs to finish it —
// the user code and the address to approve it at — as `key=value` lines on
// stdout. It then waits for the approval exactly as the window does, and exits
// when the token has been collected.
//
// The reasoning for both halves of that — why a sub-command rather than a
// global flag, and why putting the code on stdout does not weaken sign-in —
// is in signin.cpp, next to the code it justifies.
namespace OmnuvSignIn
{

// Starts the exchange and returns. The caller is expected to run the event
// loop afterwards; everything from here on is driven by it, and the process
// ends with QCoreApplication::exit().
//
// The usage errors — a bad option, no deployment address, `--help` — are
// answered before returning, and answered by ending the process, because
// there is no event loop yet for QCoreApplication::exit() to unwind.
void start(const QStringList& args, QObject* parent);

// What `signin --as <email>` does with a token already held, once the wait for
// the account's name is over (H17).
enum class HeldToken {
    Already,    // it is that account's: nothing to do
    Replace,    // Core named another account: sign out and grant a fresh one
    Grant,      // Core refused the token (401), which already signed out
    Unknown,    // Core did not say: keep the token and fail, changing nothing
};
HeldToken judgeHeldToken(const QString& who, bool stillSignedIn, const QString& expect);

}
