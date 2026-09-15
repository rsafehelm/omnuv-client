# Changes made by Omnuv

This is a modified version of [Moonlight](https://github.com/moonlight-stream/moonlight-qt),
which is free software under the GNU General Public License version 3. Section
5 of that licence asks a modified work to say prominently that it was changed,
and when. This file is that notice.

Upstream's copyright and licence are unchanged and remain in place. "Omnuv" is
our own name and mark; the upstream project does not endorse this build.

| Date | Upstream base | What changed |
|---|---|---|
| 2026-09-09 | `14c26d8c` (master, 8 Sep 2026) | Forked. |
| 2026-09-09 | `14c26d8c` | Added a check that fails the build when files outside a fixed budget differ from upstream. |
| 2026-09-09 | `14c26d8c` | Removed upstream's Dependabot configuration: this fork pins submodules to the upstream commit it is based on. |
| 2026-09-10 | `14c26d8c` | Added an Omnuv sign-in and machine list, and opened on it instead of the host grid. All new code is under `app/omnuv/`. |
| 2026-09-10 | `14c26d8c` | Connect: a streamed machine is added by its private name and handed to the app view; an ordinary one opens a terminal. |
| 2026-09-10 | `14c26d8c` | The private network: the application joins this device to it and reports its state, without ever showing a key. |
| 2026-09-15 | `14c26d8c` | The tray says whether the shell took its icon — one `tray=ready` or `tray=unavailable:` line per run — and waits for a notification area that is still starting rather than giving up at launch. |
| 2026-09-15 | `14c26d8c` | Reduced motion and the light/dark apps theme are read from the operating system at start and again whenever it says they changed, and reported as one `motion=on|off theme=dark|light` line per run. The application has no setting of its own for either: the desktop already has both. |
| 2026-09-15 | `14c26d8c` | A `signin` action: the device-code sign-in without a window, with the code on stdout as `key=value` lines so a script can finish it. Four lines in `app/cli/commandlineparser.(cpp|h)`, because upstream's parser exits on an option it does not recognise, so only a positional action can be added; the rest is `app/omnuv/signin.cpp`. |
| 2026-09-15 | `14c26d8c` | A `Theme` singleton in `app/omnuv/Theme.qml`: the Windows geometry, type ramp, motion table and icon font in one place, reading reduced motion from the operating system through the object that already reads it. Every duration it publishes collapses to zero when the person has asked for reduced motion. Icons are named — `Theme.icon.refresh` — and the build fails if a code point appears in a view instead. |
| 2026-09-15 | `14c26d8c` | Upstream's toolbar stays inside upstream's views. One line in `app/gui/main.qml` collapses it to nothing when the StackView is showing an Omnuv view, which draws its own header; PcView, AppView, SettingsView and the segue are unchanged and keep theirs. Collapsed by height rather than by `visible`, because five upstream files assign `visible` imperatively and an assignment destroys a binding permanently. |
| 2026-09-15 | `14c26d8c` | The forced dark `#303030` in `app/gui/main.qml` becomes the window's own palette colour, so the application follows the light or dark theme the desktop was set to instead of overriding it. |
| 2026-09-15 | `14c26d8c` | On Windows the Qt Quick Controls style is Qt's own FluentWinUI3 instead of Material, and the Desktop Window Manager is asked for a Mica backdrop behind the window. Three lines in `app/main.cpp`, because a style has to be chosen before any QML imports Qt Quick Controls and that is the earliest our code runs; everything else is `app/omnuv/appearance.(h|cpp)`. Material is unchanged on every other platform. One `style=<name>` line and one `mica=requested|unavailable:<hresult>` line per run — `requested` because nothing in Windows reports whether Mica was actually drawn. Windows 10 refuses the attribute and keeps the flat surface it already had. |
| 2026-09-15 | `14c26d8c` | The Windows deploy keeps the FluentWinUI3 and Fusion QML modules. Upstream excluded both from `windeployqt` and deleted them again afterwards, because it ships Material; we set FluentWinUI3 on Windows, and it falls back to Fusion for the controls it does not implement. |
