# What install-machine.ps1 installs, and what it refuses, in a scratch root
# (its -Root mode: the files, the record, the mode; not the ACL or the
# service, which only Windows has, and which it reports as skipped).
# Run: pwsh -NoProfile -File tunnel/machine/install-machine_test.ps1
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'install-machine.ps1'
$fail = 0
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Source([string] $dirty = 'false', [switch] $tamper) {
    $dir = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid())
    New-Item -ItemType Directory -Force $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir 'onvtunneld.exe'), 'fixture daemon, not a binary', $utf8)
    [IO.File]::WriteAllText((Join-Path $dir 'wintun.dll'), 'fixture', $utf8)
    [IO.File]::WriteAllText((Join-Path $dir 'WINTUN-LICENSE.txt'), 'fixture', $utf8)
    $sha = (Get-FileHash -Algorithm SHA256 (Join-Path $dir 'onvtunneld.exe')).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText((Join-Path $dir 'onvtunneld.exe.BUILT_FROM'),
        "source=abc`ncommit=0123456789abcdef`ntree=def`ndirty=$dirty`nsha256=$sha`nbuilt=2026-10-04T00:00:00Z`n", $utf8)
    if ($tamper) { [IO.File]::WriteAllText((Join-Path $dir 'onvtunneld.exe'), 'other bytes', $utf8) }
    $dir
}

function Case($what, [bool] $ok, $source, [string] $existing = '') {
    $root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid())
    if ($existing) {
        $data = Join-Path $root 'ProgramData/onv/tunnel'
        New-Item -ItemType Directory -Force $data | Out-Null
        [IO.File]::WriteAllText((Join-Path $data $existing), '{}', $utf8)
    }
    $said = & pwsh -NoProfile -File $script -Source $source -Root $root 2>&1
    $exe = Join-Path $root 'Program Files/Omnuv/Tunnel/onvtunneld.exe'
    $mode = Join-Path $root 'ProgramData/onv/tunnel/mode'
    if ($ok) {
        $line = @($said | Where-Object { "$_" -like 'ONVTUNNEL=*' })
        $report = if ($line.Count -eq 1) { ("$($line[0])".Substring(10)) | ConvertFrom-Json } else { $null }
        if ($LASTEXITCODE -ne 0 -or -not $report) { "FAIL  ${what}: exit $LASTEXITCODE, $said"; $script:fail = 1 }
        elseif ([IO.File]::ReadAllText($mode) -ne "machine`n") { "FAIL  ${what}: mode file"; $script:fail = 1 }
        elseif ($report.mode -ne 'machine' -or $report.identity.Count -ne 0 -or $report.commit -ne '0123456789abcdef' -or
                $report.service -ne 'skipped: -Root' -or $report.data_acl -ne 'skipped: -Root') { "FAIL  ${what}: report $($line[0])"; $script:fail = 1 }
        elseif (-not (Test-Path (Join-Path $root 'Program Files/Omnuv/Tunnel/wintun.dll'))) { "FAIL  ${what}: no wintun.dll"; $script:fail = 1 }
        else { "ok    $what" }
    } else {
        if ($LASTEXITCODE -eq 0) { "FAIL  ${what}: installed: $said"; $script:fail = 1 }
        elseif ((Test-Path $exe) -or (Test-Path $mode)) { "FAIL  ${what}: refused, and left files"; $script:fail = 1 }
        else { "ok    $what ($("$said".Trim()))" }
    }
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}

Case 'a clean daemon installs in machine mode' $true (Source)
Case 'a daemon from a dirty tree is refused' $false (Source -dirty 'true')
Case 'a daemon its record does not name is refused' $false (Source -tamper)
Case 'an image with an identity is refused' $false (Source) 'config.json'
Case 'an image with a join file is refused' $false (Source) 'machine-join.json'
Case 'an image with an owner is refused' $false (Source) 'owner'
exit $fail
