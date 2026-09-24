# Omnuv, the desktop client

(`OmnuvClient`. **Omnuv Connect** is something else: the small wrapper and
installer under `packaging/connect/` that joins a device and handles
`omnuv://` links, and which installs this client on Windows.)

A desktop client for [Omnuv](https://github.com/rsafehelm/omnuv): sign in, see
your machines, click one to connect. It streams a machine that runs a streaming
recipe, opens a terminal on an ordinary one, and manages the private network
underneath without asking you to think about it.

## What this is built on

It is a modified version of **Moonlight**, the open-source game streaming
client, which does all of the hard work here: the protocol, the decoders, the
renderers, and the platform support. Upstream lives at
<https://github.com/moonlight-stream/moonlight-qt> and is worth your attention
on its own.

We add a way to sign in to Omnuv, a list of your machines, and the tunnel that
reaches them. Everything else is theirs.

Licensed under the GNU General Public License version 3, like the work it comes
from. See `CHANGES.omnuv.md` for what we changed and when, and `LICENSE` for
the terms.

## Building

The Qt application builds as upstream's does; see `README.md`. Three things
of ours come with it:

- `tunnel/build.sh` builds `onvtunneld`, the private-network daemon, a Go
  program at the root that links nothing from the application.
- On Windows the installer also needs Wintun (`wintun.dll` and its licence,
  fetched against a published digest by `lab-windows-build.yml`), the MSVC
  runtime (`scripts/fetch-vcredist`) and the WiX 7 extensions `wix/Omnuv`
  references.
- `terminal/` is a separate Rust command-line client, built with `cargo`.
