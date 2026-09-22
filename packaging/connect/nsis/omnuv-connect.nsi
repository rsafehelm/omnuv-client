; Omnuv Connect for Windows.
;
; A small installer that places the client and one command, adds a Start menu
; shortcut, and offers to join the network straight away. Everything it needs
; is inside it — the client, the service that holds the private network
; (`onvtunneld.exe`) and the adapter driver that service lazy-loads
; (`wintun.dll`). Nothing is fetched on first run, so a buyer on a locked-down
; network gets a working install rather than a download that fails at the
; moment they need it.
;
; Unsigned for now. Windows will warn on first run; the README says so plainly
; rather than pretending otherwise.

!define NAME    "Omnuv Connect"
!define PUB     "Omnuv"
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
; The build script passes an absolute path; the default is only for running
; makensis by hand from this directory.
!ifndef OUTFILE
  !define OUTFILE "OmnuvConnect-${VERSION}-setup.exe"
!endif

Name          "${NAME}"
OutFile       "${OUTFILE}"
InstallDir    "$PROGRAMFILES64\Omnuv Connect"
RequestExecutionLevel admin
Unicode       true
SetCompressor /SOLID lzma

!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

; Written into the install folder, so the uninstaller can tell a folder this
; package made from one the buyer pointed it at.
!define MARKER ".omnuv-connect-installed"

Var KeyBox
Var Key

Page directory
Page custom KeyPage KeyPageLeave
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

; One field, because there is only one thing we need from the buyer. Leaving it
; empty is allowed: the installer still places the command, and they can enrol
; later from the Start menu shortcut.
Function KeyPage
  nsDialogs::Create 1018
  Pop $0
  ${NSD_CreateLabel} 0 0 100% 36u \
    "Paste the enrolment key from your Omnuv console, under Network. It joins \
     this device to your private network and cannot be used twice."
  Pop $0
  ${NSD_CreateText} 0 44u 100% 12u ""
  Pop $KeyBox
  nsDialogs::Show
FunctionEnd

Function KeyPageLeave
  ${NSD_GetText} $KeyBox $Key
FunctionEnd

Section "Install"
  ; **The folder is always ours.** The directory page, and `/D=` on the
  ; command line, let the buyer pick any folder, and the uninstaller removes
  ; the folder with everything in it. Picking `C:\Tools` would have deleted
  ; every tool on uninstall. So a folder not already called "Omnuv Connect"
  ; gets one of that name inside it.
  ${GetFileName} "$INSTDIR" $0
  ${If} $0 != "${NAME}"
    StrCpy $INSTDIR "$INSTDIR\${NAME}"
  ${EndIf}
  SetOutPath "$INSTDIR"
  FileOpen $1 "$INSTDIR\${MARKER}" w
  FileWrite $1 "Installed by ${NAME} ${VERSION}. The uninstaller removes this folder only while this file is here.$\r$\n"
  FileClose $1
  File "omnuv-connect.ps1"

  ; **Our client, beside the wrapper.** `build.sh` stages it when it is given
  ; one (`OMNUV_CLIENT_DIR`) and defines CLIENT_DIR; without that this is a
  ; wrapper-only installer and its filename carries `-noclient` so the two are
  ; never confused. The wrapper resolves `$INSTDIR\OmnuvClient.exe` before
  ; anything on PATH, which is what stops a buyer's pre-existing upstream
  ; build from being the thing that streams — and it is also what joins the
  ; network, since the tunnel is a library inside that binary. `build.sh`
  ; refuses to build this section without both DLLs present.
!ifdef CLIENT_DIR
  File /r "${CLIENT_DIR}\*.*"
!endif

  ; A .cmd wrapper so the buyer types `omnuv-connect`, not a PowerShell
  ; invocation with an execution policy flag in it.
  FileOpen  $0 "$INSTDIR\omnuv-connect.cmd" w
  FileWrite $0 "@echo off$\r$\n"
  FileWrite $0 "powershell -NoProfile -ExecutionPolicy Bypass -File $\"%~dp0omnuv-connect.ps1$\" %*$\r$\n"
  FileClose $0

  ; Deliberately not touching the system PATH. Every safe way to do that from
  ; an installer needs a third-party plugin, and every unsafe way can truncate
  ; somebody's PATH. The Start menu shortcut opens a prompt in the right
  ; directory instead, and the installer asks for the key here so the ordinary
  ; buyer never needs a terminal at all.

  CreateDirectory "$SMPROGRAMS\Omnuv"
  CreateShortcut "$SMPROGRAMS\Omnuv\Omnuv Connect.lnk" "$SYSDIR\cmd.exe" \
    '/k "$INSTDIR\omnuv-connect.cmd"' "$SYSDIR\shell32.dll" 13

  ; **The private network's service, registered and started before anything
  ; asks it to join.** It runs as LocalSystem because creating a WireGuard
  ; adapter needs administrator rights, which this installer has and the
  ; client — running as the person, at login — does not. Measured on the rig:
  ; the identical code joins in twenty seconds from an elevated context and
  ; dies after ninety from an ordinary one.
  ;
  ; Stop first, and `config` after `create`, so an upgrade over a running
  ; install converges instead of failing: `create` refuses a name that already
  ; exists, and `config` is what moves an existing service to the new path.
  ; **The inbound rule, created here as well as by the daemon.** ICE is done by
  ; onvtunneld.exe, and with no rule for it every connectivity check times out
  ; and two peers in one city meet through a relay in another country. The MSI
  ; declares this with WiX; this installer is the other way in, so it says the
  ; same thing — same rule name, scoped by the program and never by a port,
  ; because the library picks its own port and may pick another tomorrow.
  ;
  ; `delete` first, so an upgrade from another location converges rather than
  ; leaving a rule that permits the old path. The daemon asserts the rule at
  ; every start, which is what repairs one somebody removed; this is what makes
  ; it true before the service has ever run, and `Uninstall` takes it away.
  DetailPrint "Allowing the private network through the firewall…"
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Omnuv private network"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Omnuv private network" dir=in action=allow program="$INSTDIR\onvtunneld.exe" enable=yes profile=any'
  Pop $0
  ${If} $0 != 0
    DetailPrint "The firewall rule was not created. The private network will still work; connections may take a relay until the service repairs it."
  ${EndIf}

  DetailPrint "Installing the private network service…"
  nsExec::ExecToLog 'sc.exe stop OnvTunnel'
  Pop $0
  nsExec::ExecToLog 'sc.exe create OnvTunnel binPath= "$INSTDIR\onvtunneld.exe" start= auto DisplayName= "Omnuv private network"'
  Pop $0
  nsExec::ExecToLog 'sc.exe config OnvTunnel binPath= "$INSTDIR\onvtunneld.exe" start= auto'
  Pop $0
  nsExec::ExecToLog 'sc.exe description OnvTunnel "Holds this device on its Omnuv private network."'
  Pop $0
  nsExec::ExecToLog 'sc.exe start OnvTunnel'
  Pop $0
  ${If} $0 != 0
    DetailPrint "The private network service did not start. Omnuv will install, but this device cannot reach your machines by name until it does."
  ${EndIf}

  ; Enrol now, if they gave us a key. This is the whole point of the installer:
  ; the buyer never opens a terminal.
  ${If} $Key != ""
    DetailPrint "Joining your private network…"
    nsExec::ExecToLog '"$INSTDIR\omnuv-connect.cmd" enrol "$Key"'
    Pop $0
    ${If} $0 != 0
      DetailPrint "The device did not join. Open Omnuv Connect from the Start menu to try again."
    ${EndIf}
  ${EndIf}

  ; The omnuv:// scheme, so the console's buttons open this rather than asking
  ; the buyer to copy anything.
  WriteRegStr HKLM "SOFTWARE\Classes\omnuv" "" "URL:Omnuv"
  WriteRegStr HKLM "SOFTWARE\Classes\omnuv" "URL Protocol" ""
  WriteRegStr HKLM "SOFTWARE\Classes\omnuv\DefaultIcon" "" "$SYSDIR\shell32.dll,13"
  ; **PowerShell directly, never the .cmd (H3c, 22 September 2026).** A URL
  ; handler that is a batch file hands the URL to cmd, which expands %VAR%
  ; inside it and lets a raw quote end the argument. -File passes the URL to
  ; the script as it arrived, and the script refuses one that became two.
  WriteRegStr HKLM "SOFTWARE\Classes\omnuv\shell\open\command" "" \
    '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\omnuv-connect.ps1" handle "%1"'

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OmnuvConnect" \
    "DisplayName" "${NAME}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OmnuvConnect" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OmnuvConnect" \
    "Publisher" "${PUB}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OmnuvConnect" \
    "UninstallString" "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  ; **The tunnel goes with it now.** It used to be a separate product of
  ; somebody else's, installed system-wide, and leaving somebody's network
  ; behind on an uninstall was the polite thing to do. It is our own service
  ; today, so it is stopped and removed here — and it must be *stopped before
  ; the files go*, or the directory cannot be deleted and the uninstall
  ; reports success over a service still running from a path that no longer
  ; exists.
  ;
  ; What is deliberately kept: the device's identity under
  ; C:\ProgramData\onv. Removing it would make a reinstall enrol again and
  ; leave the old peer behind in the buyer's network, which is the orphan this
  ; whole design exists to avoid. The peer is revoked from the console.
  nsExec::ExecToLog 'sc.exe stop OnvTunnel'
  Pop $0
  nsExec::ExecToLog 'sc.exe delete OnvTunnel'
  Pop $0
  ; The rule names a binary that is about to stop existing. WiX removes its own
  ; on an MSI uninstall; this is the same courtesy from the other installer.
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Omnuv private network"'
  Pop $0
  Delete "$INSTDIR\omnuv-connect.ps1"
  Delete "$INSTDIR\omnuv-connect.cmd"
  Delete "$INSTDIR\uninstall.exe"
  ; The client shipped inside this package, so it goes with it — unlike the
  ; tunnel client above, which the buyer may be using for something else.
  ; **Recursively only when the folder is provably ours**: named "Omnuv
  ; Connect" and holding the marker the installer wrote. An install made
  ; before the marker existed, or a folder somebody else owns, loses only the
  ; files named above, and the folder stays if anything else is in it.
  ${GetFileName} "$INSTDIR" $0
  ${If} $0 == "${NAME}"
  ${AndIf} ${FileExists} "$INSTDIR\${MARKER}"
    RMDir /r "$INSTDIR"
  ${Else}
    DetailPrint "Left $INSTDIR in place: it is not a folder this installer made."
    RMDir "$INSTDIR"
  ${EndIf}
  Delete "$SMPROGRAMS\Omnuv\Omnuv Connect.lnk"
  RMDir  "$SMPROGRAMS\Omnuv"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OmnuvConnect"
  DeleteRegKey HKLM "SOFTWARE\Classes\omnuv"
SectionEnd
