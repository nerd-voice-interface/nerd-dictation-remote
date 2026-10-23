#!/usr/bin/env python3
# Runs on the server host, started over ssh by the remote backend: connects
# to the nerd-dictation daemon's recognition socket (nerd_dictation_ex/
# remote_server.py: socket_path()) and copies bytes between it and stdio.
# Standard library only; sent as base64, so it passes any login shell.
import os, socket, sys, threading
path = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/run/user/%d" % os.getuid(),
                    "nerd-dictation-asr.sock")
s = socket.socket(socket.AF_UNIX)
try:
    s.connect(path)
except OSError as e:
    sys.stderr.write("no nerd-dictation recognition server at %s: %s\n" % (path, e))
    sys.exit(3)
def up():
    while True:
        d = os.read(0, 65536)
        if not d:
            s.shutdown(socket.SHUT_WR)
            return
        s.sendall(d)
threading.Thread(target=up, daemon=True).start()
while True:
    d = s.recv(65536)
    if not d:
        break
    while d:                      # os.write may write only part of it
        d = d[os.write(1, d):]
