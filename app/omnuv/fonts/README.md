# The client's fonts

The web console's two text faces (`src/console-shared/src/theme.css` in the
omnuv repository: `--font-sans`, `--font-mono`), shipped in the client so the
desktop draws what the web draws. Loaded by `appearance.cpp` before any QML;
chosen by `Theme.qml`'s `textFamily` and `monoFamily`. Both under the SIL Open
Font License 1.1 (the `OFL-*.txt` beside them, bundled in the resources).

| file | from | verified |
|---|---|---|
| `InstrumentSans-Variable.ttf` | github.com/Instrument/instrument-sans at 7fa22308a3d0c94ee2b3cd537a1196b65db34a3e, `fonts/variable/InstrumentSans[wdth,wght].ttf` (no tagged release exists) | git blob 3589b81b22d3defc725dfdcdf16b5da7c9adc691, sha256 b24f1812584816958afcf22e22d08e44318c5e51651e25d2438efdde389b33b1 |
| `OFL-InstrumentSans.txt` | the same commit, `OFL.txt` | git blob 26bd2f954bfa282d235370728095db97d8a36f6f |
| `JetBrainsMono-Regular.ttf` | JetBrainsMono-2.304.zip, release v2.304 (zip sha256 6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf), `fonts/ttf/` | git blob dff66cc50702c75abd025dcf49f62a4dcc2d72de, the repository's at tag v2.304; sha256 a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f |
| `JetBrainsMono-Medium.ttf` | the same | git blob 97671156df256e850498054fdebcd41d74a65d6b; sha256 31c92d01a8a08528b718a43addf0ad3df0af2ca4b7b3290a452f70f358e14d3d |
| `OFL-JetBrainsMono.txt` | the same zip, `OFL.txt` | git blob 8bee4148c1d54dbf5dae6d6c117fc80414266abb |

The web's display face, Bricolage Grotesque, is not shipped: headings use
Instrument Sans, the web's own fallback for it.
