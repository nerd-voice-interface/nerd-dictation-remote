#!/bin/bash
# Protocol and robustness checks against the real server code
# (nerd_dictation_ex/remote_server.py) with a fake recognizer (no GPU), over
# the local bridge (host "localhost"). Usage: protocol-test.sh LIB SAMPLES_DIR
set -u
LIB=$1; S=$2
HERE=$(cd "$(dirname "$0")" && pwd)
export XDG_RUNTIME_DIR=$(mktemp -d); chmod 700 "$XDG_RUNTIME_DIR"
export NERD_DICTATION_REMOTE_HOST=localhost
W1=$S/01-basic-hello.wav; W2=$S/02-basic-test.wav
fail=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fail=1; fi; }
server() { FAKE_DELAY=${1:-0} FAKE_LIFETIME=120 python3 "$HERE/fake_server.py" > "$XDG_RUNTIME_DIR/server.log" 2>&1 & SRV=$!; sleep 1; }
stop() { kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; rm -f "$XDG_RUNTIME_DIR/nerd-dictation-asr.sock"; }

# A. no server: fails at once, says why, nothing to type
out=$(CONNECT_WAIT=1 python3 "$HERE/client.py" "$LIB" "$W1" 2>&1)
check "no server -> error, not text" '[[ $(tail -1 <<<"$out" | cut -d" " -f3) -lt 0 ]]'
check "no server -> reason given" 'grep -q "no nerd-dictation recognition server" <<<"$out"'
check "no server -> answer within 1 s" '(( $(tail -1 <<<"$out" | cut -d" " -f2) < 1000 ))'

# B. busy: two clients at once, 2 s per segment; both get text, one waits
server 2
# (CONNECT_WAIT: a segment sent before the link is up fails at once, by design)
CONNECT_WAIT=2 python3 "$HERE/client.py" "$LIB" "$W1" > "$XDG_RUNTIME_DIR/b1" 2>/dev/null &
CONNECT_WAIT=2 python3 "$HERE/client.py" "$LIB" "$W2" > "$XDG_RUNTIME_DIR/b2" 2>/dev/null &
wait %2 %3 2>/dev/null; wait
t1=$(cut -d" " -f2 "$XDG_RUNTIME_DIR/b1"); t2=$(cut -d" " -f2 "$XDG_RUNTIME_DIR/b2")
check "busy -> both recognized" 'grep -q " 0 5.00s" "$XDG_RUNTIME_DIR/b1" && grep -q " 0 5.00s" "$XDG_RUNTIME_DIR/b2"'
check "busy -> serialized (one ~2 s, other ~4 s: $t1/$t2 ms)" '(( (t1 > 3500) != (t2 > 3500) ))'
stop

# C. client drops mid-segment: server survives, worker skips the work
server 4
CONNECT_WAIT=1 python3 "$HERE/client.py" "$LIB" "$W1" > /dev/null 2>&1 & C=$!
sleep 3; kill -9 $C; pkill -f "exec.*base64" 2>/dev/null; pkill -f "python3 -c.*nerd-dictation-asr" 2>/dev/null
sleep 5
check "drop -> server noticed" 'grep -q "client left\|dropped\|disconnected" "$XDG_RUNTIME_DIR/server.log"'
out=$(CONNECT_WAIT=1 python3 "$HERE/client.py" "$LIB" "$W2" 2>&1)
check "drop -> next client still served" 'grep -q " 0 5.00s" <<<"$out"'
stop

# D. server restarted between two segments: one reconnect, no lost segment
server 0
HERE="$HERE" python3 -c "
import sys, time, wave, signal, os
sys.path.insert(0, '$HERE'); import client
lib = client.load('$LIB'); h = lib.nerd_dictation_create(); time.sleep(1)
pcm = lambda f: (lambda w: w.readframes(w.getnframes()))(wave.open(f))
print('first', client.recognize(lib, h, pcm('$W1')), flush=True)
open('$XDG_RUNTIME_DIR/ready', 'w').close()
while not os.path.exists('$XDG_RUNTIME_DIR/restarted'): time.sleep(0.2)
print('second', client.recognize(lib, h, pcm('$W2')), flush=True)
lib.nerd_dictation_destroy(h)
" > "$XDG_RUNTIME_DIR/d" 2>&1 & D=$!
while [ ! -e "$XDG_RUNTIME_DIR/ready" ]; do sleep 0.2; done
stop; server 0; touch "$XDG_RUNTIME_DIR/restarted"; wait $D
check "restart -> second segment recognized after reconnect" 'grep -q "second (0, .5.00s.)" "$XDG_RUNTIME_DIR/d"'

# E. protocol bounds: oversized frame and wrong first frame are refused
out=$(python3 - "$XDG_RUNTIME_DIR/nerd-dictation-asr.sock" <<'PY'
import socket, struct, sys
def ask(data):
    s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.settimeout(5)
    s.sendall(data)
    try:
        return s.recv(200)
    except socket.timeout:
        return b"TIMEOUT"
print(ask(struct.pack("<IB", 50_000_000, 1)))
print(ask(struct.pack("<IB", 3, 3) + b"xy"))
print(ask(struct.pack("<IBH", 3, 1, 99)))
PY
)
check "bounds -> oversized frame refused" 'grep -q "out of range" <<<"$out"'
check "bounds -> AUDIO before HELLO refused" 'grep -q "expected HELLO" <<<"$out"'
check "bounds -> unknown protocol version refused" 'grep -q "not supported" <<<"$out"'
stop
rm -rf "$XDG_RUNTIME_DIR"
exit $fail
