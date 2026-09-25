# Who record-owner.ps1 writes as the tunnel's owner, in a scratch root.
# Run: pwsh -NoProfile -File packaging/connect/nsis/record-owner_test.ps1
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'record-owner.ps1'
$fail = 0
function Case($what, $want, [hashtable]$with, $existing) {
    $root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid())
    if ($existing) { New-Item -ItemType Directory -Force (Join-Path $root 'onv/tunnel') | Out-Null
                     Set-Content (Join-Path $root 'onv/tunnel/owner') $existing }
    $said = & pwsh -NoProfile -File $script -Root $root @with 2>&1
    if ($LASTEXITCODE -ne 0) { "FAIL  ${what}: exit $LASTEXITCODE, $said"; $script:fail = 1 }
    $f = Join-Path $root 'onv/tunnel/owner'
    $got = if (Test-Path $f) { (Get-Content -Raw $f).Trim() } else { '' }
    if ($got -eq $want) { "ok    $what" } else { "FAIL  ${what}: owner '$got', wanted '$want'"; $script:fail = 1 }
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}
$person = 'S-1-5-21-1111111111-2222222222-3333333333-1001'
Case 'the person at the desk owns it' $person @{ ConsoleSid = $person }
Case 'nobody at the desk: nobody yet' '' @{ ConsoleSid = 'nobody' }
Case 'the lookup itself fails: nobody, and the install goes on' '' @{}
Case 'SYSTEM is not a person' '' @{ ConsoleSid = 'S-1-5-18' }
Case 'an upgrade keeps the owner it has' 'S-1-5-21-9-9-9-500' @{ ConsoleSid = $person } 'S-1-5-21-9-9-9-500'
exit $fail
