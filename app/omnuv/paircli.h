#pragma once

#include <QStringList>

class QObject;

// Omnuv: pair this device with a machine, from a shell, without a person
// reading four digits off one screen and typing them into another.
//
//     OmnuvClient pair-machine <name>.internal
//
// **Why this exists, when `pair` already does.** Upstream's `pair` is the
// honest version of Moonlight's model: it prints a PIN and waits for somebody
// to type it into the streaming host's own web page. That is the last step of
// renting a machine that asks the buyer to be an administrator of it, and it
// exists only because a client has no way to prove to a machine that it is
// entitled to pair.
//
// A marketplace machine has one. It mints a single-use login for its own
// streaming host at first boot, reports it to Core through the ordinary status
// channel, and the owner's client collects it once and spends it. The window
// already does exactly this — `OmnuvView.qml` calls `generatePinString`,
// `pairComputer` and `deliverPin` in that order — and this is the same three
// calls with no window, for the end-to-end run and for anyone automating a
// fleet.
//
// **The order is the machine's rather than ours.** `pairComputer` goes first,
// because the identifier a PIN is addressed to does not exist until this
// client's pairing request is already waiting on the machine. That is why the
// delivery hangs off the launcher's `pairing` signal instead of being called
// beside it.
//
// It writes `key=value` lines on stdout and nothing else, exits 0 when the
// machine is paired and 1 when it is not.
//
//     state=pairing  machine=gpu-1-ab12cd34.internal
//     state=paired
//     state=failed  reason=<the sentence naming the obstacle>
namespace OmnuvPairCli
{

// Starts the pairing and returns. The caller runs the event loop; this ends
// the process itself through QCoreApplication::exit().
void start(const QStringList& args, QObject* parent);

}
