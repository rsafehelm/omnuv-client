#!/bin/sh
# `omnuv-connect enrol` against a stand-in Core on loopback (H3): a key is
# spent only after Core has named where it leads and the person said yes.
#
#   joins       usable, --yes: the network is named, then netbird runs
#   declined    usable, "n" typed at a terminal: netbird never runs
#   foreign     404: refused, netbird never runs
#   spent       usable false: refused
#   no answer   503: refused, since could-not-ask is not a yes
#   no tty      usable, no terminal and no --yes: refused
#   not a key   a quote in the key: refused before anything is sent
#
# And the tunnel client's install (H11), with no netbird on PATH:
#   tampered    a download that is not the pinned release: nothing installed
#   pinned      the real release (needs the network): installed, then joined
set -u
here=$(cd "$(dirname "$0")" && pwd)
script="$here/../../packaging/connect/omnuv-connect"
work=$(mktemp -d)
trap 'kill $(cat "$work/pid" 2>/dev/null) 2>/dev/null; rm -rf "$work"' EXIT
fails=0
mkdir -p "$work/bin"
printf '#!/bin/sh\necho "$@" >> "%s/netbird.log"\n' "$work" > "$work/bin/netbird"
printf '#!/bin/sh\nexec "$@"\n' > "$work/bin/sudo"
chmod +x "$work/bin/netbird" "$work/bin/sudo"

serve() { # mode
    cat > "$work/core.py" <<'PY'
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
        open(sys.argv[2], "a").write(self.path + " " + asked.get("key", "") + "\n")
        if mode == "foreign": return self.reply(404, {"error": "not found"})
        if mode == "down": return self.reply(503, {"error": "unavailable"})
        return self.reply(200, {"device": "laptop", "network": "lab-net", "project": "Render",
                                "organization": "Acme Studio", "usable": mode != "spent",
                                "expires_at": None})
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
    rm -f "$work/asked" "$work/netbird.log"
    python3 "$work/core.py" "$1" "$work/asked" > "$work/port" & echo $! > "$work/pid"
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$work/port" ] && break; sleep 0.2; done
}
stop() { kill "$(cat "$work/pid")" 2>/dev/null; rm -f "$work/port"; }
bin="$work/bin"
run() { # key, extra args… ; stdin is /dev/null, so there is no terminal
    key=$1; shift
    PATH="$bin:$PATH" timeout 120 sh "$script" enrol "$key" --management-url https://nb.example \
        --core-url "http://127.0.0.1:$(cat "$work/port")" "$@" < /dev/null > "$work/out" 2>&1
}
check() { # name, expected exit (0 or 1), netbird ran (yes/no), text
    st=$1; name=$2; want=$3; ran=$4; text=$5
    got=no; [ -s "$work/netbird.log" ] && got=yes
    exited=1; [ $st -eq 0 ] && exited=0
    if [ "$exited" = "$want" ] && [ "$got" = "$ran" ] && grep -q "$text" "$work/out"; then echo "ok    $name"
    else echo "FAIL  $name (exit $st, netbird ran: $got): $(tail -3 "$work/out")"; fails=$((fails+1)); fi
}

serve usable; run k-1234 --yes; check $? "joins, having named the network" 0 yes 'organization "Acme Studio"'
grep -q "^/v1/devices/key-info k-1234$" "$work/asked" || { echo "FAIL  the key was not asked about"; fails=$((fails+1)); }
grep -q -- "--setup-key-file" "$work/netbird.log" && ! grep -q k-1234 "$work/netbird.log" \
    || { echo "FAIL  the key reached netbird's command line"; fails=$((fails+1)); }
stop

serve usable
if command -v script >/dev/null 2>&1; then
    printf 'n\n' | PATH="$work/bin:$PATH" timeout 30 script -qec "sh '$script' enrol k-1234 --management-url https://nb.example --core-url http://127.0.0.1:$(cat "$work/port")" /dev/null > "$work/out" 2>&1
    check $? "declined at the terminal" 1 no "the key was not used"
else echo "skip  declined at the terminal (no script(1))"; fi
run k-1234; check $? "no terminal and no --yes" 1 no "run again with --yes"
stop

serve foreign; run k-1234 --yes; check $? "a key this Core did not issue" 1 no "did not issue that key"; stop
serve spent;   run k-1234 --yes; check $? "a spent key" 1 no "already been used"; stop
serve down;    run k-1234 --yes; check $? "Core not answering" 1 no "could not ask"; stop

serve usable; run 'k"}x' --yes; check $? "not a key" 1 no "does not look like a key"
[ -s "$work/asked" ] && { echo "FAIL  a malformed key was sent"; fails=$((fails+1)); }
stop

# No netbird: a sudo that writes down what it was asked, and "installs" the
# binary into this directory rather than /usr/local/bin.
mkdir -p "$work/bare"
cat > "$work/bare/sudo" <<SH
#!/bin/sh
echo "\$@" >> "$work/netbird.log"
[ "\$1" = install ] && [ "\$2" = -m ] && cp "\$4" "$work/bare/netbird"
exit 0
SH
chmod +x "$work/bare/sudo"
printf '#!/bin/sh\nfor a; do [ "$p" = -o ] && echo not-the-release > "$a"; p=$a; done\n' > "$work/tampered-curl"
chmod +x "$work/tampered-curl"

serve usable
mkdir -p "$work/tampered"; cp "$work/bare/sudo" "$work/tampered/"
# One PATH serves both requests, so the stand-in answers only the download
# and hands the key lookup to the real curl.
real_curl=$(command -v curl)
cat > "$work/tampered/curl" <<SH
#!/bin/sh
case "\$*" in *github.com*) exec "$work/tampered-curl" "\$@" ;; esac
exec "$real_curl" "\$@"
SH
chmod +x "$work/tampered/curl"
bin="$work/tampered"; run k-1234 --yes; check $? "a tampered download installs nothing" 1 no "did not match its published digest"
bin="$work/bare"
if curl -fsSI -o /dev/null https://github.com 2>/dev/null; then
    run k-1234 --yes; st=$?
    if [ $st -eq 0 ] && grep -q "^install -m 0755 .*/usr/local/bin/netbird$" "$work/netbird.log" \
        && grep -q "netbird service install" "$work/netbird.log" && grep -q -- "--setup-key-file" "$work/netbird.log"; then
        echo "ok    the pinned release installs, then joins"
    else echo "FAIL  the pinned release (exit $st): $(tail -3 "$work/out") / $(cat "$work/netbird.log" 2>/dev/null)"; fails=$((fails+1)); fi
else echo "skip  the pinned release (no network)"; fi
stop

# The bundled client (the Linux .deb, 24 September 2026): enrolment goes
# through it, the key on its standard input, and nothing else is fetched or run.
cat > "$work/client" <<SH
#!/bin/sh
echo "\$@" > "$work/client.args"
cat > "$work/client.stdin"
exit \${CLIENT_EXIT:-0}
SH
chmod +x "$work/client"
serve usable
rm -f "$work/netbird.log" "$work/client.args" "$work/client.stdin"
OMNUV_CONNECT_CLIENT="$work/client" run k-1234 --yes; st=$?
if [ $st -eq 0 ] && grep -qx "enrol --key-stdin --management-url https://nb.example" "$work/client.args" \
    && grep -qx "k-1234" "$work/client.stdin" && ! grep -q k-1234 "$work/client.args" && [ ! -s "$work/netbird.log" ]; then
    echo "ok    the bundled client joins, the key on its stdin, netbird untouched"
else echo "FAIL  the bundled client (exit $st): args=$(cat "$work/client.args" 2>/dev/null) netbird=$(cat "$work/netbird.log" 2>/dev/null) $(tail -2 "$work/out")"; fails=$((fails+1)); fi
CLIENT_EXIT=1 OMNUV_CONNECT_CLIENT="$work/client" run k-1234 --yes; check $? "the bundled client refusing is reported" 1 no "did not join"
stop

[ $fails -eq 0 ] && echo "all cases hold" || echo "$fails case(s) failed"
exit $fails
