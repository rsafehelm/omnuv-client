# Omnuv Connect

One command that joins a device to a buyer's private network. It installs the
tunnel client if it is missing, joins with the key from the console, and
optionally installs the game streaming client.

```
omnuv-connect enrol <key>            join the private network
omnuv-connect enrol <key> --gaming   and install the game streaming client
omnuv-connect status                 is it connected?
omnuv-connect leave                  disconnect this device
```

On Windows the installer asks for the key while it runs, so an ordinary buyer
never opens a terminal at all.

## The `omnuv://` links

The console emits links so a person clicks a button instead of copying a key or
remembering a machine's name. This package registers the scheme, and each
platform does it its own way:

| Platform | How the scheme is owned |
|---|---|
| Ubuntu, Debian | A desktop entry with `MimeType=x-scheme-handler/omnuv` |
| Windows | Registry keys under `HKLM\SOFTWARE\Classes\omnuv` |
| macOS | A small application bundle, because a command in `/usr/local/bin` cannot own a scheme |

```
omnuv://enrol?key=<key>                       join this device
omnuv://stream?host=<name>.internal&app=<app> open the stream
omnuv://ssh?host=<name>.internal&user=<user>  open a terminal
```

Nothing in a link is a secret the person did not already have on screen, and
anything that is not an `omnuv://` link is refused. If the package is not
installed the button does nothing and the command beside it still works.

## Building

```
./packaging/connect/build.sh 0.1.0
```

Everything is built in containers, so the host needs Docker and nothing else:
no toolchain, no root. Artifacts land in `packaging/connect/dist/`, and Core serves them
at `/downloads` so the console can hand a buyer the right one — `platform.yml`
in the private repository copies them onto the platform host.

**This moved out of that repository on 21 September 2026**, because Core never
runs a line of it: `downloads.rs` prints the command and serves the file, and
everything here executes on the buyer's machine. It sits beside the client it
packages, so `OMNUV_CLIENT_DIR` is this repository's own `build/` output
rather than a path handed in from elsewhere.

`OMNUV_MANAGEMENT_URL` sets the overlay address compiled into the package. The
address is per-deployment, so the package is too.

| Platform | Artifact | Built with |
|---|---|---|
| Ubuntu, Debian | `omnuv-connect_<v>_all.deb` | `dpkg-deb` under `fakeroot` |
| Windows | `OmnuvConnect-<v>-setup.exe` | NSIS |
| macOS | `OmnuvConnect-<v>.pkg` | `xar` plus `bomutils`, built from source |

## Two decisions worth knowing

**We fetch the third-party clients, we do not bundle them.** The tunnel client
and the streaming client are installed from their own publishers at run time.
That way a buyer gets the current version, security updates come from the people
who write them, and we are not redistributing somebody else's software under our
name. The cost is that the first run needs network, which is acceptable for a
product whose entire purpose is a network.

**Nothing is signed yet.** That is a deliberate, temporary choice, and each
platform reacts differently:

| Platform | What the buyer sees |
|---|---|
| Ubuntu | Nothing. `apt` installs a local `.deb` without complaint |
| Windows | "Windows protected your PC". More info, then Run anyway |
| macOS | Refused on a double click. Right-click, Open, then Open again |

The console says this next to each download rather than letting somebody
discover it. Signing is a purchase and a process, not a code change: an
organisation-validated certificate on a hardware token for Windows, and an Apple
developer account plus notarisation for macOS.

## What is tested, and what is not

Built and installed on a clean Ubuntu 26.04 container: the package installs, the
command is on `PATH`, and it refuses politely without a key. The Windows
installer is a valid executable and the macOS package is a valid flat package,
but **neither has been run on its own operating system**, because there is no
Windows or Mac in this lab. Someone must do that before either is offered to a
buyer.
