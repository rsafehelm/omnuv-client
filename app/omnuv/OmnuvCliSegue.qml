// Omnuv: what `OmnuvClient.exe stream <machine> "<app>"` opens.
//
// **Measured, not assumed.** On 15 September 2026 the Windows rig drove that
// exact command against a name under `.invalid` and reported `SEGUE=absent`:
// the failure was answered by upstream's Material `ErrorMessageDialog` in
// `app/gui/CliStartStreamSegue.qml`, in upstream's words. That command is not
// a developer's back door — `omnuv-connect --stream` runs it
// (`packaging/connect/omnuv-connect.ps1:63`), every `omnuv://stream` link the
// console emits runs it (`packaging/connect/omnuv-connect:150`), and the
// gaming-rig end-to-end streams with it. So a buyer who is not inside the
// widget was getting upstream's vocabulary, and a parity matrix fed by that
// harness would have read *same* on a row where the two paths differed. This
// file is the fix: the `stream` verb's initial view is ours.
//
// It is `OmnuvSegue` with a launcher attached, and deliberately nothing else.
// A second failure panel, a second ladder or a second set of sentences would
// be the second vocabulary between console and client that v0.2 refuses, one
// layer down.
//
// `launcher`, `streamHost` and `streamApp` are root context properties set
// together in `app/main.cpp`'s `StreamRequested` arm — the launcher because
// upstream publishes it that way for all three of its command-line views, the
// other two because the positional arguments are parsed there and re-parsing
// them in QML would be one rule with two implementations.

import QtQuick 2.9

OmnuvSegue {
    cli: launcher

    // There is nothing behind this view, so Escape, *Close* and a stream that
    // finished all mean *leave the program*. Upstream's `quitAfter`, by
    // another name — and not optional here: Omnuv sets
    // `setQuitOnLastWindowClosed(false)` for the tray, so a command-line run
    // that nobody quits explicitly would sit there for ever.
    quitOnLeave: true

    // A `stream` run is given an address and never asks Core what the machine
    // is called, so the name and the address are the same string. The owner is
    // unknown for the same reason; *Open a terminal instead* already hides
    // itself when `machineUser` is empty, which is its default.
    machineName: streamHost
    machineHost: streamHost

    // Replaced by the launcher's own capitalisation when it finds the
    // application (`cliSessionCreated`). Set here so the ladder and the
    // failure sentences can name it during the thirty seconds before that.
    appName: streamApp
}
