# The real remote_server.RemoteASRServer with a fake recognizer worker (no GPU):
# text = "<seconds>s", delay = $FAKE_DELAY per segment. Socket under $XDG_RUNTIME_DIR.
import os, sys, threading, time, queue
sys.path.insert(0, os.environ.get("NERD_DICTATION_SRC", os.path.expanduser("~/forge/nerd-dictation")))
from nerd_dictation_ex.remote_server import RemoteASRServer
q = queue.Queue()
def worker():
    while True:
        _, req = q.get()
        try:
            if req.cancelled:
                print("worker: skipped cancelled request", flush=True); continue
            time.sleep(float(os.environ.get("FAKE_DELAY", "0")))
            req.text = f"{len(req.audio)/32000:.2f}s"
        finally:
            req.done.set()
threading.Thread(target=worker, daemon=True).start()
srv = RemoteASRServer(lambda r: q.put((None, r)), lambda: "fake")
srv.start()
time.sleep(float(os.environ.get("FAKE_LIFETIME", "600")))
srv.stop()
