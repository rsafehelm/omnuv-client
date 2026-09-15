#include "signin.h"
#include "omnuvsession.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Why this is a sub-command and not a global flag
// ---------------------------------------------------------------------------
//
// `Moonlight.exe --print-code` cannot be made to work without taking upstream's
// option handling apart. With no positional argument, GlobalCommandLineParser
// falls straight into handleUnknownOptions(), which calls exit(1) with the help
// text on any option it has not been told about — so a bare flag of ours is a
// usage error before anything of ours has run. That trap is already recorded in
// omnuvsession.h, where `shouldStartHidden()` reads a QSettings key instead of
// an autostart flag for exactly this reason.
//
// Every existing action — pair, list, quit, stream — is a positional argument,
// and the loop that recognises them returns *before* the unknown-option check,
// which is deliberate: it is what lets a sub-command carry options the global
// parser has never heard of. So this follows them exactly. `signin` is an
// action, recognised in that same loop, and main.cpp handles it the way it
// handles `list`: no window, no QML engine, stdout is the product.
//
// It is also the honest shape. This does not start a stream or open a window;
// it is a separate small program that happens to share a binary, which is what
// a sub-command means.
//
// **It is spelled `signin`, not `--print-code`.** The mode is the one
// windows_impl.md I0 calls `--print-code`; the name changed because the parser
// decided it, and a flag that only works after an action would have been a
// second spelling for the same thing.
//
// ---------------------------------------------------------------------------
// Why putting the code on stdout does not weaken sign-in
// ---------------------------------------------------------------------------
//
// The device authorization grant carries two codes and only one of them is a
// secret.
//
//   device_code   the application's credential. Whoever holds it collects the
//                 token. It is never displayed, and it is not reachable from
//                 here: OmnuvSession keeps it private with no accessor, so
//                 nothing in this file could print it even by mistake.
//
//   user_code     the public half by construction. Its entire job is to be
//                 read off a screen and typed into a browser — Core even picks
//                 an alphabet for people who misread characters, dropping 0/O
//                 and 1/I/L. Publishing it is what it is for.
//
// And reading the code is not approving it. Approval is a POST to
// /v1/auth/device/decide, which requires a signed-in caller and records *that*
// caller as the approver, so a code on stdout still becomes a token only when
// somebody already signed in says yes. Nothing here adds a route, relaxes a
// check, or skips the browser: it is the same exchange with the same approval,
// with the code legible to a script instead of to a person squinting at a
// screenshot of the rig.
//
// What does change is who can read the code: anyone who can read this process's
// stdout. On the lab rig that is the operator's own redirect, it is the same
// exposure the screenshot already had, and a code that is read but never
// approved expires in ten minutes having done nothing.
//
// ---------------------------------------------------------------------------
// The stdout contract
// ---------------------------------------------------------------------------
//
// stdout carries the result and nothing else; logs, progress and complaints go
// to stderr. That is a house rule, and this program is the reason it matters
// here — a harness parses these lines.
//
//   user-code=ABCD-EFGH
//   verification-uri=https://<console>/connect
//   signed-in=yes                 (or signed-in=already, if there was a token)
//
// Parsed by splitting on the *first* `=`: a verification URI may carry a query
// string, and no key ever contains one. Values are never quoted or escaped —
// neither a user code nor a URL contains a newline.
//
// **`signed-in=` is the terminator.** Its presence is what says the block is
// complete, so a harness that reads a partial stdout cannot mistake it for a
// finished sign-in. Failure writes nothing further to stdout: it goes to stderr
// and to the exit status.
//
//   0   signed in — freshly, or there was already a token on this device
//   1   usage, or no deployment address to sign in to
//   2   the exchange failed: refused, expired, or Core was unreachable
//
// Nothing else writes to stdout on this path. The logger writes to the log
// file, or to stderr when stderr was redirected, and never to stdout — that is
// main.cpp's doing and it is why this works at all. Upstream's `list` writes to
// stdout too, but it is a different action and cannot be running; within
// app/omnuv/ this file is the only writer, which is checked rather than
// remembered by the step in .github/workflows/omnuv-change-budget.yml.

namespace OmnuvSignIn
{

namespace {

// Getting a code is one round trip to Core: it works in a moment or it is not
// going to. Waiting for a person to approve it is bounded by the ten minutes
// Core gives the code, and we let Core's own expiry answer first — this
// deadline exists for the case where nothing answers at all.
const int kCodeMs = 60 * 1000;
const int kApprovalMs = 11 * 60 * 1000;

// Flushed on every line, and that is not belt-and-braces. On the rig the client
// is started with its output redirected to a file (`omnuv-run.ps1`), and the
// CRT block-buffers a redirected stream: unbuffering is done for a console in
// main.cpp, and a redirect gets nothing.
//
// main.cpp does flush both streams before returning — upstream added that for
// exactly this reason, for `list` — but a flush at exit is no use here. The
// whole point of these lines is that something reads them *while this process
// is still waiting*, up to eleven minutes before it exits. And the early exits
// below leave through ::exit(), which does not reach that cleanup at all.
void fact(const char* key, const QString& value)
{
    fprintf(stdout, "%s=%s\n", key, qPrintable(value));
    fflush(stdout);
}

void complain(const QString& text)
{
    fprintf(stderr, "%s\n", qPrintable(text));
}

// No Q_OBJECT: every connection below lands in a lambda with this object as
// its context, so the class declares no signals or slots of its own and needs
// no moc pass — which keeps the whole of the sign-in in one .cpp instead of a
// header and a generated file.
class Driver : public QObject
{
public:
    Driver(QObject* parent, OmnuvSession* session)
        : QObject(parent), m_session(session)
    {
        m_deadline.setSingleShot(true);

        // The status line is a sentence in a person's words about what just
        // happened, and it is exactly the running commentary a failed run needs
        // to be diagnosable. It is commentary, so it belongs on stderr.
        connect(m_session, &OmnuvSession::statusChanged, this, [this]() {
            if (!m_session->status().isEmpty()) {
                complain(m_session->status());
            }
        });

        connect(m_session, &OmnuvSession::pendingChanged, this, [this]() {
            // Order matters, and it is OmnuvSession::poll()'s order: on
            // success the token is set, then the pending code is cleared —
            // which emits this — and only then signedInChanged. So a cleared
            // code while signed in is the *success* path arriving early, and
            // must not be read as the attempt ending.
            if (m_session->signedIn()) {
                return;
            }

            if (!m_session->userCode().isEmpty()) {
                fact("user-code", m_session->userCode());
                fact("verification-uri", m_session->verificationUri());
                m_printed = true;
                m_deadline.start(kApprovalMs);
                return;
            }

            // The code went away without a token: refused, expired, or the
            // request for it failed. OmnuvSession has already said which on
            // stderr. (signIn() also clears the pending state on the way in,
            // before there is anything to print — hence the guard.)
            if (m_printed) {
                finish(2);
            }
        });

        // **A request that concluded without a code is the answer, not a
        // reason to keep waiting.** `signIn()` makes one request and retries
        // nothing: `busy` falls exactly once, and if no code arrived with it —
        // unreachable, or a deployment that offered none — the sixty-second
        // deadline would only repeat what the session already said, a minute
        // later. On the rig that minute was a third of every cycle. Deferred
        // one turn of the loop so the status the session sets in the same call
        // has been printed by the handler above before this exits.
        connect(m_session, &OmnuvSession::busyChanged, this, [this]() {
            if (m_session->busy() || m_printed || !m_session->userCode().isEmpty()) {
                return;
            }
            QTimer::singleShot(0, this, [this]() { finish(2); });
        });

        connect(m_session, &OmnuvSession::signedInChanged, this, [this]() {
            if (m_session->signedIn()) {
                fact("signed-in", QStringLiteral("yes"));
                finish(0);
            }
        });

        connect(&m_deadline, &QTimer::timeout, this, [this]() {
            complain(m_printed
                         ? QStringLiteral("No approval within %1 minutes; the code has expired.")
                               .arg(kApprovalMs / 60000)
                         : QStringLiteral("No sign-in code within %1 seconds.").arg(kCodeMs / 1000));
            finish(2);
        });
    }

    void begin()
    {
        m_deadline.start(kCodeMs);
        m_session->signIn();
    }

private:
    // Both signals can arrive for one outcome, so the exit happens once —
    // the same reason tunnel.cpp fires its process callback once.
    void finish(int code)
    {
        if (m_done) {
            return;
        }
        m_done = true;
        m_deadline.stop();
        QCoreApplication::exit(code);
    }

    OmnuvSession* m_session;
    QTimer m_deadline;
    bool m_printed = false;
    bool m_done = false;
};

} // namespace

void start(const QStringList& args, QObject* parent)
{
    // Upstream's CommandLineParser — the one that gives pair and list their
    // help, version and unknown-option handling — is private to
    // commandlineparser.cpp. This action takes no options at all, so the
    // handful of lines a plain QCommandLineParser needs is cheaper than
    // exporting that class, and behaves the same on everything it can meet.
    QCommandLineParser parser;
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    parser.setApplicationDescription(
        "\n"
        "Sign this device in to Omnuv without a window.\n"
        "\n"
        "Writes the sign-in code and the address to approve it at as key=value\n"
        "lines on stdout, then waits for the approval and exits. Everything\n"
        "else — progress, and anything that went wrong — goes to stderr.\n"
        "\n"
        "The deployment comes from OMNUV_CORE_URL, or from the address this\n"
        "client was last pointed at.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("signin", "Sign this device in");

    // Deliberately not parser.process(): that prints to stdout on error, and
    // stdout is the machine-readable channel here. Upstream's own helper makes
    // the same split — information to stdout, errors to stderr — so a caller
    // reading only stdout sees results and never a complaint.
    if (!parser.parse(args)) {
        complain(parser.errorText());
        ::exit(1);
    }
    // --help and --version put their answer on stdout and end the process, the
    // same way every other action's parser does. They are the one case where a
    // caller asked for something other than the key=value block, so they are
    // not a second writer of the result — they are a different invocation.
    if (parser.isSet(QStringLiteral("help"))) {
        fprintf(stdout, "%s", qPrintable(parser.helpText()));
        ::exit(0);
    }
    if (parser.isSet(QStringLiteral("version"))) {
        parser.showVersion();
    }
    if (!parser.unknownOptionNames().isEmpty()) {
        complain(QStringLiteral("Unknown options: %1").arg(parser.unknownOptionNames().join(", ")));
        ::exit(1);
    }

    // Constructed directly rather than through the QML singleton, so no tray
    // is created and no window is involved: the factory in omnuvsession.cpp is
    // what builds those, and it only runs when QML resolves the singleton.
    auto* session = new OmnuvSession(parent);

    if (session->coreUrl().isEmpty()) {
        complain(QStringLiteral(
            "No Omnuv deployment to sign in to. Set OMNUV_CORE_URL, or sign in "
            "once from the window."));
        ::exit(1);
    }

    // A token already here is the correct answer to "sign this device in", and
    // signing in again would mint a second one — which nothing would hold, and
    // which the console has no surface to revoke. Said as a distinct value
    // rather than as `yes`, so a harness can tell a fresh grant from a device
    // that was already enrolled.
    if (session->signedIn()) {
        fact("signed-in", QStringLiteral("already"));
        ::exit(0);
    }

    // Everything above ends the process itself, because QCoreApplication::exit()
    // does nothing before the event loop it is meant to unwind has started.
    // From here on there is a loop, and Driver uses it.
    (new Driver(parent, session))->begin();
}

} // namespace OmnuvSignIn
