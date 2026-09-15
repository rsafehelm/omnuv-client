#pragma once

#include <QString>

// Where the bearer token lives.
//
// **This was the one thing about the client marked as needing to change before
// release, and residency is what made it urgent.** A program you open, use and
// close holds a token for minutes. A widget holds one across reboots, on a
// laptop that gets lost, in a file any process running as that user can read.
// `%APPDATA%\Omnuv\token` at mode 0600 is honest protection against another
// *user*, and none at all against another program.
//
// So: the platform's own credential store, which on Windows is bound to the
// user's login and is what a person can see and revoke in Credential Manager
// without needing us to provide a button for it.
//
//   Windows  Credential Manager (CredWriteW / CredReadW), generic credential
//   macOS    Keychain — not implemented
//   Linux    libsecret — not implemented; the file remains, and says so
//
// The two unimplemented ones fall back to the old file rather than failing to
// sign in. That is a deliberate, stated compromise and not an oversight: it is
// written down here, in `TODO.md`, and in the fallback path itself.
namespace OmnuvCredentials
{
    // The stored token, or an empty string. On Windows this also migrates a
    // token found in the old file: written to Credential Manager, then the
    // file is deleted. A plaintext copy left behind after an upgrade would be
    // the worst of both designs.
    QString load();

    // True when the token was actually stored. A caller that ignores this and
    // signs the person out on the next launch is worse than one that says so.
    bool store(const QString& token);

    void clear();

    // Whether this build uses the platform store rather than the file. The
    // About box and any support conversation should be able to say which.
    bool usingPlatformStore();
}
