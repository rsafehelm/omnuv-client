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
//
// **One slot per Core, keyed by its origin (22 September 2026).** There was one
// slot, so a device signed in to production and pointed at the test Core either
// sent production's token to the test Core, which answered 401 and deleted it,
// or signed out first and lost it. A token is now only ever read for the origin
// it was issued at, so moving between deployments keeps each one's sign-in.
//
//   Windows   `Omnuv Connect/<origin>`
//   file      `<config>/omnuv/tokens/<first 16 hex of sha256(origin)>`
//
// The single slot of earlier versions, `Omnuv Connect` and `<config>/omnuv/token`,
// is migrated once, and only into the slot of the origin it was saved beside
// (`legacyOwner`): a token is never guessed into a Core it was not issued by.
namespace OmnuvCredentials
{
    // The origin a token is keyed by: scheme and host lower-cased, a default
    // port dropped, no path, query, fragment or user info. Empty for an
    // address that is not http(s) with a host.
    QString origin(const QString& coreUrl);

    // Whether a token may be sent to this Core (H2, 22 September 2026):
    // https with a host, or plain http to this machine's own loopback, which
    // is where the fixtures and a Core started by hand listen. Anything else
    // would put a token that never expires on the wire in the clear, and the
    // front door's redirect to https would then make it work, so nothing
    // would look wrong.
    bool secureCore(const QString& coreUrl);

    // The token stored for this origin, or an empty string. When there is none
    // and `legacyOwner` equals `origin`, a token in the old single slot is
    // moved here first: written to this origin's slot, then the old one
    // removed, because a copy left in both would be the risk of both designs.
    QString load(const QString& origin, const QString& legacyOwner = QString());

    // True when the token was actually stored. A caller that ignores this and
    // signs the person out on the next launch is worse than one that says so.
    bool store(const QString& origin, const QString& token);

    // True only when this origin's slot is absent afterwards. Other origins'
    // slots are untouched.
    bool clear(const QString& origin);

    // Whether this build uses the platform store rather than the file. The
    // About box and any support conversation should be able to say which.
    bool usingPlatformStore();
}
