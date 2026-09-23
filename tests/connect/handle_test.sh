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
{ echo "$p"; for a in "\$@"; do echo "[\$a]"; done; } > "$work/called"
STUB
    chmod +x "$work/bin/$p"
done
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

# What the console sends.
opens   'omnuv://stream?host=gpu-1-ab12cd34.internal&app=Desktop' 'moonlight [stream] [gpu-1-ab12cd34.internal] [Desktop]'
opens   'omnuv://stream?host=rig.internal'                          'moonlight [stream] [rig.internal]'
opens   'omnuv://stream?host=rig.internal&app=Steam%20Big%20Picture' 'moonlight [stream] [rig.internal] [Steam Big Picture]'
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
refuses 'omnuv://join'                                      'Get command'
refuses 'omnuv://wipe?host=x'                               'unknown link'
refuses 'https://example.com/'                              'not an omnuv link'

[ $fails -eq 0 ] && echo "all cases hold" || echo "$fails case(s) failed"
exit $fails
