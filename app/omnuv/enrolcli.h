#pragma once

#include <QStringList>

class QObject;

// Omnuv: join this device to its private network from a shell.
//
//     OmnuvClient enrol <key> --management-url <url>    first time, with a key
//     OmnuvClient enrol --management-url <url>          afterwards, no key
//
// The same two steps the window takes, and the same distinction: a device that
// enrolled before comes back up on the identity it already holds and creates
// nothing, and only a device that has never enrolled spends a key. Passing a
// key to a device that did not need one leaves a dead peer in somebody's
// network, which is why the key is optional here rather than required.
//
// **Why this exists at all.** Until the tunnel was embedded, enrolment was
// `netbird up` driven by the packaging wrapper, and everything that wanted a
// device on the network — the installer's key page, the console's *Connect
// this device* button, the end-to-end run — went through that command. The
// command is gone; this is what those now call, so the client that owns the
// tunnel is also the thing that joins with it.
//
// It writes `key=value` lines on stdout and nothing else, exits 0 when the
// device is on the network and 1 when it is not.
namespace OmnuvEnrol
{

// Starts the join and returns. The caller runs the event loop; this ends the
// process itself through QCoreApplication::exit().
//
// Usage errors are answered before returning, by ending the process, because
// there is no event loop yet to unwind.
void start(const QStringList& args, QObject* parent);

}
