# Installs onvtunneld on a Windows marketplace image, in machine mode.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File install-machine.ps1 -Source <dir>
#
# Run by the image build as SYSTEM or an elevated administrator, before
# sysprep. <dir> holds what tunnel/build.sh puts in dist/x64: onvtunneld.exe,
# onvtunneld.exe.BUILT_FROM, and beside them wintun.dll and WINTUN-LICENSE.txt
# (the client build's copies; Wintun travels with its notices).
#
# **The daemon alone, not the client's MSI** (W3, 4 October 2026). The MSI
# installs the desktop client, the omnuv:// handlers, the Start menu, and a
# custom action that records whoever sits at the console as the tunnel's
# owner. A machine needs none of it, and the last is exactly what machine mode
# exists to refuse. A property on the MSI would have to switch all of that off
# and every MSI build would carry the switch; four files and a service are
# what the image needs, so four files and a service are what this installs.
#
# What it leaves:
#
#   %ProgramFiles%\Omnuv\Tunnel\   onvtunneld.exe, wintun.dll, WINTUN-LICENSE.txt
#   %ProgramData%\onv\tunnel\      SYSTEM and Administrators only, inherited;
#                                  `mode` = machine; no identity, no join file
#   service OnvTunnel              automatic, LocalSystem, restarted on failure;
#                                  not started: its first start is the clone's
#
# The agent's join file arrives in that directory at the clone's first boot
# (cloudbase-init write_files) and inherits its ACL: tunnel/machine.go.
#
# Refuses, and installs nothing, when the daemon is not the one its
# BUILT_FROM names, when it was built from a dirty tree, when the data
# directory already holds an identity or a join file (an image carries
# neither), or when a service by the name already exists.
#
# -Root <dir> installs into <dir> instead of the system's directories and
# skips the two steps only Windows has (the ACL and the service), saying so:
# the half the tests run (install-machine_test.ps1).
#
# Prints one line, `ONVTUNNEL=<json>`, the facts the image build asserts.

param(
    [Parameter(Mandatory = $true)][string] $Source,
    [string] $Root = ''
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Refuse([string] $why) {
    [Console]::Error.WriteLine("install-machine: $why")
    exit 2
}

$windows = -not $Root
if ($windows) {
    if ($env:OS -ne 'Windows_NT') { Refuse 'not Windows; -Root is the test mode' }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Refuse 'needs SYSTEM or an elevated administrator' }
    $installDir = Join-Path $env:ProgramFiles 'Omnuv\Tunnel'
    $dataDir = Join-Path $(if ($env:ProgramData) { $env:ProgramData } else { 'C:\ProgramData' }) 'onv\tunnel'
} else {
    $installDir = Join-Path $Root 'Program Files/Omnuv/Tunnel'
    $dataDir = Join-Path $Root 'ProgramData/onv/tunnel'
}

# 1. The daemon is the one its record names, from a clean tree.
$exe = Join-Path $Source 'onvtunneld.exe'
$record = Join-Path $Source 'onvtunneld.exe.BUILT_FROM'
foreach ($f in @($exe, $record, (Join-Path $Source 'wintun.dll'), (Join-Path $Source 'WINTUN-LICENSE.txt'))) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { Refuse "missing $f" }
}
$built = @{}
foreach ($line in [IO.File]::ReadAllLines($record)) {
    $k, $v = $line -split '=', 2
    if ($k) { $built[$k.Trim()] = "$v".Trim() }
}
if ($built['dirty'] -ne 'false') { Refuse "the daemon was built from a dirty tree (dirty=$($built['dirty']))" }
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLowerInvariant()
if ($sha -ne $built['sha256']) { Refuse "onvtunneld.exe is $sha, its BUILT_FROM names $($built['sha256'])" }

# 2. An image carries no identity and no key.
foreach ($name in @('config.json', 'state.json', 'machine', 'deployments', 'active', 'owner', 'machine-join.json')) {
    if (Test-Path -LiteralPath (Join-Path $dataDir $name)) { Refuse "$dataDir already holds ${name}: an image carries no identity" }
}
if ($windows -and (Get-Service -Name 'OnvTunnel' -ErrorAction SilentlyContinue)) { Refuse 'a service named OnvTunnel already exists' }

# 3. The files.
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
foreach ($name in @('onvtunneld.exe', 'wintun.dll', 'WINTUN-LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $Source $name) -Destination (Join-Path $installDir $name) -Force
}
$installed = Join-Path $installDir 'onvtunneld.exe'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $installed).Hash.ToLowerInvariant() -ne $sha) { Refuse 'the copied daemon differs from its source' }

# 4. The data directory: SYSTEM and Administrators, inherited by what
# cloudbase-init writes into it later. The daemon sets the same DACL at every
# start (securedir_windows.go); this one exists before the daemon ever ran.
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
$acl = 'skipped: -Root'
if ($windows) {
    & icacls.exe $dataDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE -ne 0) { Refuse "icacls exited $LASTEXITCODE on $dataDir" }
    $acl = (Get-Acl -LiteralPath $dataDir).Sddl
}
[IO.File]::WriteAllText((Join-Path $dataDir 'mode'), "machine`n", $utf8)

# 5. The service, not started.
$service = 'skipped: -Root'
if ($windows) {
    New-Service -Name 'OnvTunnel' -BinaryPathName ('"' + $installed + '"') -DisplayName 'Omnuv private network' `
        -Description "Holds this machine's WireGuard adapter for its buyer's Omnuv private network." `
        -StartupType Automatic | Out-Null
    & sc.exe failure OnvTunnel reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null
    if ($LASTEXITCODE -ne 0) { Refuse "sc.exe failure exited $LASTEXITCODE" }
    $svc = Get-CimInstance Win32_Service -Filter "Name='OnvTunnel'"
    $service = [ordered]@{ start_mode = $svc.StartMode; account = $svc.StartName; state = $svc.State; path = $svc.PathName }
}

$report = [ordered]@{
    sha256       = $sha
    commit       = $built['commit']
    source       = $built['source']
    install_dir  = $installDir
    data_dir     = $dataDir
    mode         = ([IO.File]::ReadAllText((Join-Path $dataDir 'mode'))).Trim()
    data_acl     = $acl
    service      = $service
    identity     = @(Get-ChildItem -LiteralPath $dataDir -Force | Where-Object { $_.Name -ne 'mode' } | ForEach-Object Name)
}
'ONVTUNNEL=' + ($report | ConvertTo-Json -Compress -Depth 4)
