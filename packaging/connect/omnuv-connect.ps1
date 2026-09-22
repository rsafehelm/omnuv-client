# Omnuv Connect — joins this device to your private network.
#
# One command, and it installs nothing. The key comes from your Omnuv console,
# under Network; the client in this package does the joining and says whether
# it worked.
#
#   omnuv-connect enrol <key>            join the private network
#   omnuv-connect handle omnuv://…       what the console's buttons open
#
# **The streaming client is ours and ships with this package.** It used to be
# fetched with `winget install MoonlightGameStreamingProject.Moonlight`, which
# installed *upstream's* build over the top of the product: a client with no
# Omnuv view, no tray, no sign-in and no segue — none of I0-I4. `-Gaming` is
# gone with it, because there is nothing left to opt into. A buyer who follows
# the product's own path must end up running the client this repository
# builds, and `gaming-rig-e2e` step 6 would otherwise have certified a stream
# driven by a different program.
#
# **And the tunnel is ours too now, so this script installs nothing.** It used
# to fetch the vendor's Windows installer and drive its `netbird` command;
# `client/embed` from the pinned 0.78.1 is compiled into `onvtunnel.dll` and
# ships beside `OmnuvClient.exe`, which loads it by name. So `enrol` forwards
# to the client, and the two verbs that had no destination any more are gone:
#
#   status   the window shows it, live, in the row that says so. There was
#            nothing left for a shell to ask — the state lives in the process
#            that holds the tunnel, and that process is the client.
#   leave    the tunnel goes down when the client exits, because it is the
#            client. `netbird down` had a daemon to talk to; this does not.
param(
    [Parameter(Position = 0)][string]$Command = 'help',
    [Parameter(Position = 1)][string]$Key,
    [string]$ManagementUrl = '@MANAGEMENT_URL@'
)

$ErrorActionPreference = 'Stop'
$Version = '@VERSION@'

function Say($m) { Write-Host $m }
function Die($m) { Write-Error "omnuv-connect: $m"; exit 1 }

# Ours, beside this script. The same resolution `Start-Stream` does, and for
# the same reason: nothing on PATH is allowed to stand in for the client this
# package shipped.
function Client-Path {
    $exe = Join-Path $PSScriptRoot 'OmnuvClient.exe'
    if (-not (Test-Path $exe)) { Die 'the Omnuv client is missing from this installation: reinstall Omnuv Connect' }
    return $exe
}

function Start-Stream($HostName, $App) {
    # Verified against the client's own parser: stream <host> "<app>".
    # **Ours, beside this script, before anything on PATH.** `Get-Command
    # moonlight` finds upstream's build if a buyer happens to have one, and
    # streaming through that would exercise a client with none of our work in
    # it — which is exactly what the winget line used to guarantee.
    $exe = Client-Path
    if ($App) { Start-Process -FilePath $exe -ArgumentList @('stream', $HostName, $App) }
    else      { Start-Process -FilePath $exe -ArgumentList @('stream', $HostName) }
}

function Start-Ssh($HostName, $User) {
    $target = if ($User) { "$User@$HostName" } else { $HostName }
    # Windows ships OpenSSH, so this needs nothing installed. `cmd.exe` keeps
    # the window open after ssh exits, and it parses its command line, which is
    # why nothing reaches it that `Assert-Link*` has not already checked.
    Start-Process -FilePath 'cmd.exe' -ArgumentList @('/k', 'ssh', $target)
}

# **A link is input from any web page, not from the console.** Anything can
# emit omnuv://, and the scheme is registered machine-wide, so every value is
# checked against what the console actually sends before it reaches a program.
# `omnuv://ssh?host=x%26calc` used to become `cmd.exe /k ssh x&calc`, and a
# host starting with `-` becomes an ssh option (`-oProxyCommand=…`).
# `-cmatch` with `\A…\z`: `-match` ignores case and lets `$` match before a
# trailing newline.
function Assert-LinkHost($HostName) {
    $label = '[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?'
    if ($HostName.Length -gt 253 -or $HostName -cnotmatch "\A$label(?:\.$label)*\z") {
        Die "that link names a machine this will not open: $HostName"
    }
}
function Assert-LinkUser($User) {
    if ($User -and $User -cnotmatch '\A[A-Za-z_][A-Za-z0-9_.-]{0,31}\z') {
        Die "that link names a user this will not open: $User"
    }
}
function Assert-LinkApp($App) {
    if ($App -and ($App.Length -gt 128 -or $App -cmatch '\A-|["\x00-\x1f\x7f]')) {
        Die "that link names an application this will not open: $App"
    }
}

# A link that enrols replaces this device's network membership, so a click is
# not consent: the person is asked, and anything but Yes stops it.
function Confirm-LinkEnrol {
    Add-Type -AssemblyName PresentationFramework
    $answer = [System.Windows.MessageBox]::Show(
        "A link asks to join this device to an Omnuv network.`n`nThis replaces the network it is on now. Only continue if you started this from your Omnuv console.",
        'Omnuv Connect', 'YesNo', 'Warning', 'No')
    if ($answer -ne 'Yes') { Die 'joining was cancelled' }
}

# The console emits omnuv:// links so a person clicks a button instead of
# copying a key or remembering a machine's name.
function Invoke-Link($Url) {
    if ($Url -notmatch '^omnuv://') { Die "not an omnuv link: $Url" }
    $rest   = $Url -replace '^omnuv://', ''
    $action = ($rest -split '\?', 2)[0].TrimEnd('/')
    $query  = if ($rest -match '\?') { ($rest -split '\?', 2)[1] } else { '' }
    $q = @{}
    foreach ($pair in ($query -split '&')) {
        if ($pair -match '=') {
            $kv = $pair -split '=', 2
            $q[$kv[0]] = [System.Uri]::UnescapeDataString($kv[1] -replace '\+', ' ')
        }
    }
    switch ($action.ToLower()) {
        { $_ -in 'enrol', 'enroll', 'join' } {
            Confirm-LinkEnrol
            # **No key: the app joins with its own sign-in** (22 September
            # 2026). The console's links carried a key, which the legacy
            # `enrol <key>` spent without writing a membership record, so the
            # app then called its own network "another or unverified" and
            # offered to move it. Without a key the client asks Core for one as
            # the signed-in account, exactly as its Join button does, and
            # records the membership. A key is still accepted, for a device
            # where no one has signed in to the app.
            if ($q['key']) { & $PSCommandPath enrol $q['key'] }
            else {
                & (Client-Path) enrol
                if ($LASTEXITCODE -ne 0) { Die 'the device did not join. Open Omnuv and sign in, then use Join this device.' }
            }
        }
        'stream' {
            if (-not $q['host']) { Die 'that link names no machine' }
            Assert-LinkHost $q['host']
            Assert-LinkApp $q['app']
            Start-Stream $q['host'] $q['app']
        }
        'ssh' {
            if (-not $q['host']) { Die 'that link names no machine' }
            Assert-LinkHost $q['host']
            Assert-LinkUser $q['user']
            Start-Ssh $q['host'] $q['user']
        }
        default { Die "unknown link: omnuv://$action" }
    }
}

switch ($Command.ToLower()) {
    'handle'  { Invoke-Link $Key }
    { $_ -in 'enrol', 'enroll', 'join' } {
        if (-not $Key) { Die 'usage: omnuv-connect enrol <key>   (copy it from your Omnuv console)' }
        if (-not $ManagementUrl -or $ManagementUrl -eq '@MANAGEMENT_URL@') {
            Die 'no network address was built into this package; pass -ManagementUrl'
        }
        Say 'Joining your private network…'
        # **Forwarded, not reimplemented.** The client holds the tunnel, so it
        # is the only thing that can join with it. It prints `state=` lines and
        # exits non-zero if the device did not end up on the network; both are
        # passed straight through rather than summarised, because its sentence
        # names the obstacle and a summary of ours would not.
        & (Client-Path) enrol $Key --management-url $ManagementUrl
        if ($LASTEXITCODE -ne 0) { Die 'the device did not join. Check the key has not already been used.' }
        Say ''
        Say 'Connected. Your machines are reachable by name, for example:'
        Say '    ssh gpu-1.internal'
        Say '    or open Omnuv Connect, which lists them by name'
        Say ''
        Say 'This device now appears in your Omnuv console under Network, where you can revoke it.'
    }
    'version' { Say "omnuv-connect $Version" }
    default   { Get-Content $PSCommandPath | Select-Object -Skip 1 -First 7 | ForEach-Object { $_ -replace '^# ?', '' } }
}
