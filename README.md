# nerd-dictation-remote

A [nerd-dictation](https://github.com/nerd-voice-interface/nerd-dictation)
ASR backend that recognizes on another machine: the frontend records,
segments, postprocesses and types locally; each finished segment is sent
over ssh to the nerd-dictation daemon of the host named by
`NERD_DICTATION_REMOTE_HOST` (set from `[remote] host` in the frontend
config), which recognizes it with its own loaded model and sends the text
back. See `docs/remote.md` in nerd-dictation for the design and protocol.

```sh
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
sudo install -Dm755 build/libnerd-dictation-remote.so.1.0.0 /usr/lib/   # or makepkg -si
```

Client config (`~/.config/nerd-dictation/config.toml`):

```toml
[general]
backend = "remote"

[remote]
host = "corentin-asr"     # an ssh alias; BatchMode, so key auth only
```

Server: any nerd-dictation daemon with a local backend serves
`$XDG_RUNTIME_DIR/nerd-dictation-asr.sock`. For an unattended client, give
it a key that can only start the bridge (`src/bridge.py`; the package
installs it as `/usr/share/nerd-dictation-remote/nerd-dictation-asr-bridge`,
corentin has it as `~/.local/bin/nerd-dictation-asr-bridge`):

```
restrict,command="/home/USER/.local/bin/nerd-dictation-asr-bridge" ssh-ed25519 AAAA... client
```

Tests: `tests/protocol-test.sh build/libnerd-dictation-remote.so
<dir with 16 kHz WAVs>` runs the real server code with a fake recognizer
(no GPU): unreachable server, two clients at once, a client dropping
mid-segment, a server restart between segments, and protocol bounds.
`tests/client.py` drives the backend's C API with WAV files.
