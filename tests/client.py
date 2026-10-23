#!/usr/bin/env python3
"""Drive libnerd-dictation-remote.so through its C API, as the frontend does
(feed one segment, finalize), and print the text and the round-trip time.

  NERD_DICTATION_REMOTE_HOST=corentin client.py LIB file.wav [file.wav ...]
  prints one line per file: "<seconds of audio> <ms> <rc> <text or error>"
"""
import ctypes, os, sys, time, wave


class Info(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("name", ctypes.c_char_p),
                ("streaming", ctypes.c_int), ("sample_rate", ctypes.c_int),
                ("channels", ctypes.c_int), ("bits_per_sample", ctypes.c_int)]


class Result(ctypes.Structure):
    _fields_ = [("text", ctypes.c_char_p), ("is_final", ctypes.c_int),
                ("confidence", ctypes.c_float)]


def load(path):
    lib = ctypes.CDLL(path)
    lib.nerd_dictation_create.restype = ctypes.c_void_p
    lib.nerd_dictation_destroy.argtypes = [ctypes.c_void_p]
    lib.nerd_dictation_get_info.argtypes = [ctypes.c_void_p, ctypes.POINTER(Info)]
    lib.nerd_dictation_feed.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    lib.nerd_dictation_finalize.argtypes = [ctypes.c_void_p, ctypes.POINTER(Result)]
    lib.nerd_dictation_get_error.argtypes = [ctypes.c_void_p]
    lib.nerd_dictation_get_error.restype = ctypes.c_char_p
    return lib


def recognize(lib, handle, pcm: bytes):
    buf = ctypes.create_string_buffer(pcm, len(pcm))
    rc = lib.nerd_dictation_feed(handle, buf, len(pcm) // 2)
    if rc < 0:
        return rc, lib.nerd_dictation_get_error(handle).decode()
    r = Result()
    rc = lib.nerd_dictation_finalize(handle, ctypes.byref(r))
    if rc < 0:
        return rc, lib.nerd_dictation_get_error(handle).decode()
    return rc, r.text.decode()


def main():
    lib = load(sys.argv[1])
    handle = lib.nerd_dictation_create()
    assert handle, "create failed"
    wait = float(os.environ.get("CONNECT_WAIT", "0"))
    time.sleep(wait)                 # let the keeper connect first
    for f in sys.argv[2:]:
        w = wave.open(f)
        assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2
        pcm = w.readframes(w.getnframes())
        t = time.monotonic()
        rc, text = recognize(lib, handle, pcm)
        ms = (time.monotonic() - t) * 1000
        print(f"{len(pcm) / 32000:.2f} {ms:.0f} {rc} {text}", flush=True)
    lib.nerd_dictation_destroy(handle)


if __name__ == "__main__":
    main()
