#!/bin/sh
# The link handler, run as a browser runs it: `omnuv-connect handle <url>`.
#
# **A link is input from any web page**, so what matters is what reaches a
# program. The programs are stubs on PATH that write down how they were called;
# a case passes when the right program got exactly the right arguments, or
# when nothing was called and the handler said why.
#
#   sh tests/connect/handle_test.sh        exit 0 when every case holds
set -u
here=$(cd "$(dirname "$0")" && pwd)
script="$here/../../packaging/connect/omnuv-connect"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"
# A stub records its name and each argument on its own line, then stops.
for p in moonlight x-terminal-emulator netbird; do
    cat > "$work/bin/$p" <<STUB
#!/bin/sh
{ echo "$p"; for a in "\$@"; do echo "[\$a]"; done; } >> "$work/called"
STUB
    chmod +x "$work/bin/$p"
done
# The bundled client, a stub as well: it records its call, and answers
# `open-instance` with the exit status FAKE_OPEN names (3: off the network).
cat > "$work/bin/OmnuvClient" <<STUB
#!/bin/sh
{ echo "OmnuvClient"; for a in "\$@"; do echo "[\$a]"; done; } >> "$work/called"
[ "\${1:-}" = open-instance ] && exit "\${FAKE_OPEN:-0}"
exit 0
STUB
chmod +x "$work/bin/OmnuvClient"
OMNUV_CONNECT_CLIENT="$work/bin/OmnuvClient"
export OMNUV_CONNECT_CLIENT
# Only the stubs and the shell's own tools: nothing real can be opened.
PATH="$work/bin:/usr/bin:/bin"
export PATH

fails=0
run() { rm -f "$work/called"; sh "$script" handle "$1" > "$work/out" 2>&1; }
called() { [ -f "$work/called" ] && tr '\n' ' ' < "$work/called"; }

opens() { # url, expected call
    run "$1"
    got=$(called)
    if [ "$got" = "$2 " ]; then echo "ok    $1"; else echo "FAIL  $1 -> ${got:-nothing} ($(cat "$work/out"))"; fails=$((fails+1)); fi
}
refuses() { # url, a word the refusal must contain
    run "$1"; st=$?
    if [ -f "$work/called" ]; then echo "FAIL  $1 opened $(called)"; fails=$((fails+1))
    elif [ $st -eq 0 ]; then echo "FAIL  $1 was accepted silently"; fails=$((fails+1))
    elif ! grep -qi -- "$2" "$work/out"; then echo "FAIL  $1 refused without saying '$2': $(cat "$work/out")"; fails=$((fails+1))
    else echo "ok    $1 refused"; fi
}

# What the console sends since the Instances redesign: an instance by its id.
id=6c1e0a4b-1f2e-4d3c-9b8a-7f6e5d4c3b2a
opens   "omnuv://stream?instance=$id"                               "OmnuvClient [stream-instance] [$id]"
opens   "omnuv://open?instance=$id"                                 "OmnuvClient [open-instance] [$id]"
# **Open, refused off the network**: the app says the device is not on the
# instance's network (3), and the handler opens the app, which offers Join;
# no page is opened.
FAKE_OPEN=3; export FAKE_OPEN
opens   "omnuv://open?instance=$id"                                 "OmnuvClient [open-instance] [$id] OmnuvClient"
unset FAKE_OPEN
# A refusal the app explains (1) is passed on, and the window is not opened.
FAKE_OPEN=1; export FAKE_OPEN
run "omnuv://open?instance=$id"; st=$?
if [ $st -eq 1 ] && [ "$(called)" = "OmnuvClient [open-instance] [$id] " ]; then echo "ok    open refused by the app"; else echo "FAIL  open refused by the app: $st $(called)"; fails=$((fails+1)); fi
unset FAKE_OPEN

# What older links send, by name, still accepted (bundled client absent).
OMNUV_CONNECT_CLIENT=/nonexistent; export OMNUV_CONNECT_CLIENT
opens   'omnuv://stream?host=gpu-1-ab12cd34.internal&app=Desktop' 'moonlight [stream] [gpu-1-ab12cd34.internal] [Desktop]'
opens   'omnuv://stream?host=rig.internal'                          'moonlight [stream] [rig.internal]'
opens   'omnuv://stream?host=rig.internal&app=Steam%20Big%20Picture' 'moonlight [stream] [rig.internal] [Steam Big Picture]'
refuses "omnuv://open?instance=$id"                                 'needs the Omnuv app'
OMNUV_CONNECT_CLIENT="$work/bin/OmnuvClient"; export OMNUV_CONNECT_CLIENT
opens   'omnuv://ssh?host=web-1.internal&user=omnuv'                 'x-terminal-emulator [-e] [ssh] [omnuv@web-1.internal]'
opens   'omnuv://ssh?host=web-1.internal'                            'x-terminal-emulator [-e] [ssh] [web-1.internal]'

# What any other page could send.
refuses 'omnuv://ssh?host=-oProxyCommand=touch%20/tmp/x'   'will not open'
refuses 'omnuv://ssh?host=a%0Ab.internal'                   'will not open'
refuses 'omnuv://ssh?host=web.internal&user=-oProxy'        'will not open'
refuses 'omnuv://stream?host=rig.internal&app=-x'           'will not open'
refuses 'omnuv://stream?host=rig.internal&app=a%22b'        'will not open'
refuses 'omnuv://stream'                                    'no machine'
refuses 'omnuv://join?key=ABCDEF'                           'no longer carry'
# With no app installed, a join link says how instead.
OMNUV_CONNECT_CLIENT=/nonexistent; export OMNUV_CONNECT_CLIENT
refuses 'omnuv://join'                                      'Get command'
OMNUV_CONNECT_CLIENT="$work/bin/OmnuvClient"; export OMNUV_CONNECT_CLIENT
refuses 'omnuv://open?instance=../../etc'                    'will not open'
refuses 'omnuv://open?instance=6c1e0a4b-1f2e-4d3c-9b8a-7f6e5d4c3b2a%0Ax' 'will not open'
refuses 'omnuv://stream?instance=-oProxy'                    'will not open'
refuses 'omnuv://open'                                       'no instance'
refuses 'omnuv://wipe?host=x'                               'unknown link'
refuses 'https://example.com/'                              'not an omnuv link'

[ $fails -eq 0 ] && echo "all cases hold" || echo "$fails case(s) failed"
exit $fails
