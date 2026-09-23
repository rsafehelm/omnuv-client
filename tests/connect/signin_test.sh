#!/bin/sh
# `omnuv-connect sign-in` against a stand-in Core on loopback (H20, H23).
#
#   passing   the first poll gets a 503 and the second the token: signed in
#   refused   Core answers 400 with its reason: it ends, in Core's words
set -u
here=$(cd "$(dirname "$0")" && pwd)
script="$here/../../packaging/connect/omnuv-connect"
work=$(mktemp -d)
trap 'kill $(cat "$work/pid" 2>/dev/null) 2>/dev/null; rm -rf "$work"' EXIT
fails=0

serve() { # mode
    cat > "$work/core.py" <<'PY'
import http.server, json, sys
mode, polls = sys.argv[1], [0]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, body):
        b = json.dumps(body).encode()
        self.send_response(code); self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        self.rfile.read(int(self.headers.get("content-length", 0)))
        if self.path == "/v1/auth/device":
            return self.reply(200, {"device_code": "d", "user_code": "ABCD-1234",
                                    "verification_uri": "http://127.0.0.1/connect", "interval": 1, "expires_in": 30})
        polls[0] += 1
        if mode == "refused":
            return self.reply(400, {"error": "that code has already been used"})
        if polls[0] == 1:
            return self.reply(503, {"error": "unavailable"})
        return self.reply(200, {"token": "omnuv_pat_fixture"})
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
    python3 "$work/core.py" "$1" > "$work/port" & echo $! > "$work/pid"
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$work/port" ] && break; sleep 0.2; done
}
stop() { kill "$(cat "$work/pid")" 2>/dev/null; rm -f "$work/port"; }

serve passing
HOME="$work/home" timeout 30 sh "$script" sign-in "http://127.0.0.1:$(cat "$work/port")" > "$work/out" 2>&1; st=$?
if [ $st -eq 0 ] && grep -q "still waiting" "$work/out" && grep -q "Signed in" "$work/out"; then echo "ok    a 503 in passing, then signed in"
else echo "FAIL  a 503 in passing ended the sign-in (exit $st): $(tail -2 "$work/out")"; fails=$((fails+1)); fi
stop

serve refused
HOME="$work/home2" timeout 30 sh "$script" sign-in "http://127.0.0.1:$(cat "$work/port")" > "$work/out" 2>&1; st=$?
if [ $st -ne 0 ] && grep -q "already been used" "$work/out"; then echo "ok    a refusal ends it in Core's words"
else echo "FAIL  refusal (exit $st): $(tail -2 "$work/out")"; fails=$((fails+1)); fi
stop

[ $fails -eq 0 ] && echo "all cases hold" || echo "$fails case(s) failed"
exit $fails
