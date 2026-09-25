<#
.SYNOPSIS
  Records the person at this device's desk as the owner of its Omnuv tunnel.

.DESCRIPTION
  The operator's decision of 25 September 2026: whoever installed Omnuv
  Connect owns its tunnel, so no other account on a shared machine can claim
  it first (the tunnel answers only its owner and administrators, H4).

  On Windows the installer runs elevated, and elevation from a standard
  account runs it as the administrator who typed their password, not as the
  person at the desk. So the owner is the interactive console user, the
  account the desktop and the client run as, by SID, which is what the
  service compares callers against. Nobody at the console names nobody, and
  the first change claims it as before. An owner already recorded is never
  replaced.

  -Root and -ConsoleSid are for the test beside this file.
#>
param(
    [string]$Root = $env:ProgramData,
    [string]$ConsoleSid
)
$ErrorActionPreference = 'Stop'
$dir = Join-Path (Join-Path $Root 'onv') 'tunnel'
$owner = Join-Path $dir 'owner'

if ((Test-Path $owner) -and ((Get-Content -Raw $owner).Trim())) {
    'omnuv-connect: this device''s network already has an owner; it is kept'
    exit 0
}
if (-not $PSBoundParameters.ContainsKey('ConsoleSid')) {
    # A lookup that fails (no CIM on this system, WMI stopped) names nobody:
    # it must never fail the install.
    # Plain statements, not `$x = try {…}`: the installer runs this with
    # Windows PowerShell 5.1, not 7.
    $user = $null
    try { $user = (Get-CimInstance Win32_ComputerSystem).UserName } catch { $user = $null }
    if ($user) {
        try {
            $ConsoleSid = ([System.Security.Principal.NTAccount]$user).Translate(
                [System.Security.Principal.SecurityIdentifier]).Value
        } catch {
            "omnuv-connect: the person at the desk ($user) could not be named; the first account to join a network will own it"
            exit 0
        }
    }
}
if (-not $ConsoleSid -or $ConsoleSid -notmatch '^S-1-5-21-[0-9-]+$') {
    'omnuv-connect: installed with nobody at the desk; the first account to join a network will own it'
    exit 0
}
New-Item -ItemType Directory -Force -Path $dir | Out-Null
if ($env:OS -eq 'Windows_NT') {
    # SYSTEM and Administrators only, as the service sets it on every start
    # (securedir_windows.go), so there is no moment when it is readable.
    & icacls.exe $dir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
}
[System.IO.File]::WriteAllText($owner, "$ConsoleSid`n")
"omnuv-connect: this device's network belongs to $ConsoleSid"
