#!/bin/bash
# End to end through the real nerd-dictation frontend with backend "remote":
# a private daemon instance (own XDG_RUNTIME_DIR, HOME and config) whose
# microphone is a fake pw-cat playing WAV files in real time, typing to
# stdout. Prints what it typed. Usage: frontend-e2e.sh ND_SRC HOST WAV...
set -u
ND=$1; HOST=$2; shift 2
T=$(mktemp -d); mkdir -p "$T/home/.config/nerd-dictation" "$T/rt" "$T/bin"; chmod 700 "$T/rt"
cat > "$T/home/.config/nerd-dictation/config.toml" <<CONF
[general]
backend = "remote"
input_method = "stdout"
[remote]
host = "$HOST"
[audio]
source = "pw-cat"
sample_rate = 16000
chunk_size = 8000
[wake_word]
enabled = false
[spell_correction]
enabled = true
model_path = "/home/$(id -un)/.local/share/jamspell/ru.bin"
CONF
# Fake microphone: silence until $T/go, then the WAVs (with 1 s pauses),
# then silence; paced at real time.
cat > "$T/bin/pw-cat" <<PY
#!/usr/bin/env python3
import os, sys, time, wave
out = sys.stdout.buffer
chunk = 4000                      # 0.125 s at 16 kHz s16
def play(data):
    for i in range(0, len(data), chunk * 2):
        out.write(data[i:i + chunk * 2]); out.flush(); time.sleep(chunk / 16000)
while not os.path.exists("$T/go"):
    play(b"\\0" * chunk * 2)
for f in "$*".split():
    w = wave.open(f); play(w.readframes(w.getnframes())); play(b"\\0" * 32000)
open("$T/played", "w").close()
while True:
    play(b"\\0" * chunk * 2)
PY
chmod +x "$T/bin/pw-cat"
export HOME="$T/home" XDG_RUNTIME_DIR="$T/rt" XDG_DATA_HOME="/home/$(id -un)/.local/share"
export PATH="$T/bin:$PATH" LD_LIBRARY_PATH="$ND/build"
"$ND/build/nerd-dictation" daemon > "$T/out" 2> "$T/err" & D=$!
for i in $(seq 1 60); do [ -S "$T/rt/nerd-dictation-ex.sock" ] && break; sleep 0.5; done
sleep 3                                   # the remote link comes up
"$ND/build/nerd-dictation" begin > /dev/null 2>&1
touch "$T/go"
for i in $(seq 1 120); do [ -e "$T/played" ] && break; sleep 0.5; done
sleep 3
"$ND/build/nerd-dictation" end > /dev/null 2>&1
sleep 5
kill $D; wait $D 2>/dev/null
echo "--- typed:"; cat "$T/out"; echo
echo "--- daemon log (remote, recognizer):"
grep -E "\[remote\]|распознано|ошибка|Error|Traceback" "$T/err" | head -20
rm -rf "$T"
