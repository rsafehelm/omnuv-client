# `omnuv-connect.ps1 enrol` against a stand-in Core on loopback (H3), the same
# cases as enrol_test.sh: a key reaches the client only after Core has named
# where it leads and the person said yes.
#
# A stand-in OmnuvClient.exe beside a copy of the script records whether it
# ran. The Yes/No window is Windows-only, so here "no window" is the refusal
# case; the window itself is checked on the Windows rig.
#
#   pwsh -NoProfile -File tests/connect/enrol_test.ps1     exit 0 when every case holds
$ErrorActionPreference = 'Stop'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("onv-enrol-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
Copy-Item (Join-Path $PSScriptRoot '../../packaging/connect/omnuv-connect.ps1') $work
$client = Join-Path $work 'OmnuvClient.exe'
$ran = Join-Path $work 'client.ran'
Set-Content -Path $client -Value "#!/bin/sh`ncat > '$ran'`necho `"`$@`" >> '$ran'"
chmod +x $client
$script = Join-Path $work 'omnuv-connect.ps1'
$core = Join-Path $work 'core.py'
Set-Content -Path $core -Value @'
import http.server, json, sys
mode = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, body):
        b = json.dumps(body).encode()
        self.send_response(code); self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        asked = json.loads(self.rfile.read(int(self.headers.get("content-length", 0))))
        if mode == "foreign": return self.reply(404, {"error": "not found"})
        if mode == "down": return self.reply(503, {"error": "unavailable"})
        return self.reply(200, {"device": "laptop", "network": "lab-net", "project": "Render",
                                "organization": "Acme Studio", "usable": mode != "spent", "expires_at": None})
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
'@

$fails = 0
function Case($name, $mode, [switch]$Yes, $want, [switch]$Runs) {
    $port = Join-Path $work 'port'
    Remove-Item $port, $ran -ErrorAction SilentlyContinue
    $p = Start-Process python3 -ArgumentList $core, $mode -RedirectStandardOutput $port -PassThru
    for ($i = 0; $i -lt 25 -and -not ((Test-Path $port) -and (Get-Content $port)); $i++) { Start-Sleep -Milliseconds 200 }
    $url = "http://127.0.0.1:$((Get-Content $port).Trim())"
    $out = try {
        if ($Yes) { & $script enrol k-1234 -ManagementUrl https://nb.example -CoreUrl $url -Yes 6>&1 2>&1 | Out-String }
        else      { & $script enrol k-1234 -ManagementUrl https://nb.example -CoreUrl $url 6>&1 2>&1 | Out-String }
    } catch { $_.ToString() }
    Stop-Process -Id $p.Id
    $didRun = Test-Path $ran
    if ($didRun -eq [bool]$Runs -and $out -match [regex]::Escape($want)) { "ok    $name" }
    else { "FAIL  $name (client ran: $didRun): $out"; $script:fails++ }
}

Case 'joins, having named the network' usable -Yes 'organization "Acme Studio"' -Runs
if ((Get-Content $ran -Raw) -notmatch 'k-1234' -or (Get-Content $ran -Raw) -notmatch '--key-stdin') { "FAIL  the key did not reach the client on stdin"; $fails++ }
Case 'no window and no -Yes'          usable      'run again with -Yes'
Case 'a key this Core did not issue'  foreign -Yes 'did not issue that key'
Case 'a spent key'                    spent -Yes   'already been used'
Case 'Core not answering'             down -Yes    'could not ask'

Remove-Item -Recurse -Force $work
if ($fails -eq 0) { 'all cases hold' } else { "$fails case(s) failed" }
exit $fails
