# The Windows link handler, run as the protocol handler runs it:
# `omnuv-connect.ps1 handle <url>`. The same cases as handle_test.sh.
#
# `Start-Process` is replaced by a function that writes down what it was
# asked to start — a function outranks a cmdlet of the same name — and a
# stand-in OmnuvClient.exe sits beside a copy of the script, so nothing real
# is ever opened. Refusals arrive as errors, since the script runs with
# $ErrorActionPreference = 'Stop'.
#
#   pwsh -NoProfile -File tests/connect/handle_test.ps1     exit 0 when every case holds
$ErrorActionPreference = 'Stop'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("onv-handle-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
Copy-Item (Join-Path $PSScriptRoot '../../packaging/connect/omnuv-connect.ps1') $work
Set-Content -Path (Join-Path $work 'OmnuvClient.exe') -Value 'stand-in'
$script = Join-Path $work 'omnuv-connect.ps1'
$global:called = $null
function global:Start-Process {
    param([string]$FilePath, [string[]]$ArgumentList)
    $global:called = ((Split-Path $FilePath -Leaf), (($ArgumentList | ForEach-Object { "[$_]" }) -join ' ')) -join ' '
}

$fails = 0
function Run($url) {
    $global:called = $null
    try { & $script handle $url; return '' } catch { return $_.ToString() }
}
function Opens($url, $want) {
    $err = Run $url
    if ($global:called -eq $want) { "ok    $url" }
    else { "FAIL  $url -> $($global:called) $err"; $script:fails++ }
}
function Refuses($url, $word) {
    $err = Run $url
    if ($global:called) { "FAIL  $url opened $($global:called)"; $script:fails++ }
    elseif (-not $err) { "FAIL  $url was accepted silently"; $script:fails++ }
    elseif ($err -notmatch [regex]::Escape($word)) { "FAIL  $url refused without saying '$word': $err"; $script:fails++ }
    else { "ok    $url refused" }
}

# What the console sends.
Opens   'omnuv://stream?host=gpu-1-ab12cd34.internal&app=Desktop'  'OmnuvClient.exe [stream] [gpu-1-ab12cd34.internal] [Desktop]'
Opens   'omnuv://stream?host=rig.internal'                          'OmnuvClient.exe [stream] [rig.internal]'
Opens   'omnuv://stream?host=rig.internal&app=Steam%20Big%20Picture' 'OmnuvClient.exe [stream] [rig.internal] [Steam Big Picture]'
Opens   'omnuv://ssh?host=web-1.internal&user=omnuv'                 'cmd.exe [/k] [ssh] [omnuv@web-1.internal]'
Opens   'omnuv://ssh?host=web-1.internal'                            'cmd.exe [/k] [ssh] [web-1.internal]'

# What any other page could send.
Refuses 'omnuv://ssh?host=-oProxyCommand=calc'         'will not open'
Refuses 'omnuv://ssh?host=x%26calc'                     'will not open'
Refuses 'omnuv://ssh?host=web.internal&user=-oProxy'    'will not open'
Refuses 'omnuv://stream?host=rig.internal&app=-x'       'will not open'
Refuses 'omnuv://stream?host=rig.internal&app=a%22b'    'will not open'
Refuses 'omnuv://stream'                                'no machine'
Refuses 'omnuv://join?key=ABCDEF'                       'no longer carry'
Refuses 'omnuv://wipe?host=x'                           'unknown link'
Refuses 'https://example.com/'                          'not an omnuv link'

Remove-Item -Recurse -Force $work
if ($fails -eq 0) { 'all cases hold' } else { "$fails case(s) failed" }
exit $fails
