#!/usr/bin/env python3
"""A server that accepts and never answers HELLO: destroy() must return at
once (it cuts the handshake short), and finalize() during that handshake
must fail at once instead of waiting for it. Usage: silent-server-test.py LIB"""
import ctypes, os, socket, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import client

rt = tempfile.mkdtemp()
os.chmod(rt, 0o700)
os.environ["XDG_RUNTIME_DIR"] = rt
os.environ["NERD_DICTATION_REMOTE_HOST"] = "localhost"
srv = socket.socket(socket.AF_UNIX)
srv.bind(os.path.join(rt, "nerd-dictation-asr.sock"))
srv.listen(4)
held = []
threading.Thread(target=lambda: [held.append(srv.accept()) for _ in range(4)], daemon=True).start()

lib = client.load(sys.argv[1])
fail = 0
h = lib.nerd_dictation_create()
time.sleep(1)                       # the keeper is now inside the handshake
t = time.monotonic()
rc, text = client.recognize(lib, h, b"\0\0" * 16000)
ms = (time.monotonic() - t) * 1000
ok = rc < 0 and ms < 1000
print(f"{'PASS' if ok else 'FAIL'}: finalize during a stuck handshake fails at once ({ms:.0f} ms, rc {rc}: {text})")
fail |= not ok
t = time.monotonic()
lib.nerd_dictation_destroy(h)
ms = (time.monotonic() - t) * 1000
ok = ms < 3000
print(f"{'PASS' if ok else 'FAIL'}: destroy during a stuck handshake returns at once ({ms:.0f} ms)")
fail |= not ok
sys.exit(fail)
