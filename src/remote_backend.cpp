/*
 * nerd-dictation "remote" backend: recognition on another machine.
 *
 * Implements the backend C API (nerd_dictation/backend_api.h) for a
 * frontend that captures, segments and types locally: feed() collects one
 * segment, finalize() sends it to the nerd-dictation daemon on the host
 * named by NERD_DICTATION_REMOTE_HOST (an ssh alias; the C API passes no
 * parameters) and returns its text. The daemon there recognizes with its
 * already loaded model (nerd_dictation_ex/remote_server.py).
 *
 * Transport: one long-lived `ssh -T <host> python3 -c ...` running the stdio
 * bridge (bridge.py) to the daemon's unix socket; ssh does authentication
 * and encryption, and reaches hosts behind any ProxyJump. For host
 * "localhost" the bridge runs here without ssh. A keeper thread connects at
 * create and reconnects after a drop; finalize() fails at once (the
 * frontend then types nothing) while the host is unreachable.
 *
 * Protocol version 1, little-endian frames: u32 length | u8 type | payload;
 * see remote_server.py for the full description.
 */

#include "nerd_dictation/backend_api.h"
#include "bridge.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr uint16_t kProtocolVersion = 1;
constexpr int kSampleRate = 16000;
constexpr uint8_t kHello = 1, kInfo = 2, kAudio = 3, kText = 4, kError = 5;
constexpr size_t kMaxSegmentSamples = 30 * kSampleRate;
constexpr uint32_t kMaxFrame = 1 << 20;
// ssh through a jump host plus the server's own wait for its recognizer.
constexpr int kHandshakeMs = 20000;
constexpr int kReplyMs = 75000;
constexpr int kRetryMinSeconds = 2, kRetryMaxSeconds = 30;

std::string base64(const std::string& in) {
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        out += t[v >> 18]; out += t[v >> 12 & 63]; out += t[v >> 6 & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += t[v >> 18]; out += t[v >> 12 & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8;
        out += t[v >> 18]; out += t[v >> 12 & 63]; out += t[v >> 6 & 63]; out += '=';
    }
    return out;
}

// One running bridge: a child process (ssh, or python3 for localhost) whose
// stdin and stdout are one end of a socketpair - so writes use MSG_NOSIGNAL
// and a dead peer is an error, not a SIGPIPE in the frontend.
struct Link {
    pid_t pid = -1;
    int fd = -1;          // our end of the socketpair
    int err_fd = -1;      // the child's stderr, for error messages
    std::string server;   // the server's backend name from INFO

    bool alive() const { return pid > 0; }

    void close_down() {
        if (fd >= 0) { ::close(fd); fd = -1; }
        if (pid > 0) {
            // TERM, up to 2 s to go, then KILL: a stuck ssh must not hang
            // the caller (the frontend's worker, or its shutdown).
            ::kill(pid, SIGTERM);
            bool gone = false;
            for (int i = 0; i < 20 && !gone; ++i) {
                pid_t r = ::waitpid(pid, nullptr, WNOHANG);
                gone = r == pid || (r < 0 && errno == ECHILD);
                if (!gone) ::usleep(100000);
            }
            if (!gone) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
            }
            pid = -1;
        }
        if (err_fd >= 0) { ::close(err_fd); err_fd = -1; }
    }

    // The child has exited (reaped here, or by a host that reaps all its
    // children: then waitpid says ECHILD).
    bool exited() {
        if (pid <= 0) return true;
        pid_t r = ::waitpid(pid, nullptr, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) { pid = -1; return true; }
        return false;
    }

    // What the child said on stderr so far (non-blocking).
    std::string child_stderr() const {
        std::string out;
        if (err_fd < 0) return out;
        char buf[512];
        ssize_t n;
        while ((n = ::read(err_fd, buf, sizeof(buf))) > 0 && out.size() < 2048)
            out.append(buf, n);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }
};

bool write_all(int fd, const void* data, size_t size, int timeout_ms) {
    const char* p = static_cast<const char*>(data);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (size) {
        ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) { p += n; size -= n; continue; }
        if (n < 0 && errno != EAGAIN && errno != EINTR) return false;
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        pollfd p_{fd, POLLOUT, 0};
        if (::poll(&p_, 1, left) < 0 && errno != EINTR) return false;
    }
    return true;
}

bool read_all(int fd, void* data, size_t size,
              std::chrono::steady_clock::time_point deadline) {
    char* p = static_cast<char*>(data);
    while (size) {
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        pollfd p_{fd, POLLIN, 0};
        int r = ::poll(&p_, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        ssize_t n = ::recv(fd, p, size, MSG_DONTWAIT);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return false;
        }
        p += n; size -= n;
    }
    return true;
}

bool send_frame(int fd, uint8_t type, const void* payload, size_t size) {
    uint8_t header[5];
    uint32_t length = static_cast<uint32_t>(size + 1);
    std::memcpy(header, &length, 4);   // little-endian hosts only (x86, LoongArch)
    header[4] = type;
    return write_all(fd, header, 5, kReplyMs) &&
           (size == 0 || write_all(fd, payload, size, kReplyMs));
}

// Returns false on a broken or malformed stream.
bool recv_frame(int fd, uint8_t& type, std::string& payload, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t header[5];
    if (!read_all(fd, header, 5, deadline)) return false;
    uint32_t length;
    std::memcpy(&length, header, 4);
    if (length < 1 || length > kMaxFrame) return false;
    type = header[4];
    payload.resize(length - 1);
    return payload.empty() || read_all(fd, payload.data(), payload.size(), deadline);
}

}  // namespace

struct nerd_dictation_backend {
    std::string host;
    std::vector<int16_t> segment;
    std::string text;
    std::string error;

    std::mutex mutex;            // guards link and last_failure
    Link link;
    std::string last_failure;    // why the host is unreachable, for finalize
    std::condition_variable wake;
    std::atomic<bool> stopping{false};
    std::atomic<pid_t> connecting_pid{-1};
    std::thread keeper;

    // Start a bridge and do the handshake, without holding the mutex: the
    // handshake may take seconds. connecting_pid lets destroy() cut it short.
    bool open_link(Link& link, std::string& failure) {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
            failure = std::string("socketpair: ") + std::strerror(errno);
            return false;
        }
        int ep[2];
        if (::pipe2(ep, O_CLOEXEC) < 0) {
            ::close(sv[0]); ::close(sv[1]);
            failure = std::string("pipe: ") + std::strerror(errno);
            return false;
        }
        std::string bridge = std::string("python3 -c \"import base64;exec(base64.b64decode('")
                             + base64(kBridgePy) + "'))\"";
        std::vector<std::string> args;
        if (host == "localhost") {
            args = {"python3", "-c", kBridgePy};
        } else {
            args = {"ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                    "-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=3",
                    host, bridge};
        }
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
        posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
        posix_spawn_file_actions_adddup2(&fa, ep[1], 2);
        posix_spawn_file_actions_addclosefrom_np(&fa, 3);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        // Its own process group: a Ctrl+C meant for the frontend's terminal
        // must not kill the link.
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        pid_t pid;
        int rc = ::posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);
        ::close(sv[1]);
        ::close(ep[1]);
        if (rc != 0) {
            ::close(sv[0]); ::close(ep[0]);
            failure = std::string("cannot run ") + argv[0] + ": " + std::strerror(rc);
            return false;
        }
        ::fcntl(ep[0], F_SETFL, O_NONBLOCK);
        link.pid = pid;
        link.fd = sv[0];
        link.err_fd = ep[0];
        connecting_pid = pid;
        struct Clear { std::atomic<pid_t>& p; ~Clear() { p = -1; } } clear{connecting_pid};

        uint8_t type;
        std::string payload;
        uint16_t version = kProtocolVersion;
        if (!send_frame(link.fd, kHello, &version, sizeof(version)) ||
            !recv_frame(link.fd, type, payload, kHandshakeMs)) {
            std::string why = link.child_stderr();
            failure = host + " unreachable" + (why.empty() ? "" : ": " + why);
            link.close_down();
            return false;
        }
        if (type == kError) {
            failure = host + ": " + payload;
            link.close_down();
            return false;
        }
        uint16_t server_version;
        uint32_t rate;
        if (type != kInfo || payload.size() < 7 ||
            (std::memcpy(&server_version, payload.data(), 2), server_version) != kProtocolVersion ||
            (std::memcpy(&rate, payload.data() + 2, 4), rate) != (uint32_t)kSampleRate) {
            failure = host + ": unexpected handshake reply";
            link.close_down();
            return false;
        }
        link.server = payload.substr(7);
        std::fprintf(stderr, "[remote] connected to %s (backend %s)\n",
                     host.c_str(), link.server.c_str());
        return true;
    }

    // Replace the link with a fresh one. Called without the mutex.
    bool reconnect() {
        Link fresh;
        std::string failure;
        bool ok = open_link(fresh, failure);
        std::lock_guard<std::mutex> lock(mutex);
        link.close_down();
        if (ok && !stopping) {
            link = fresh;
            last_failure.clear();
            return true;
        }
        fresh.close_down();
        last_failure = ok ? host + ": shutting down" : failure;
        return false;
    }

    // Reconnect in the background, with backoff, whenever the link is down.
    void keep() {
        int delay = kRetryMinSeconds;
        while (!stopping) {
            bool up;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (link.alive() && link.exited()) {
                    std::string why = link.child_stderr();
                    link.close_down();
                    last_failure = host + " link closed" + (why.empty() ? "" : ": " + why);
                    std::fprintf(stderr, "[remote] %s\n", last_failure.c_str());
                }
                up = link.alive();
            }
            int wait = 1;
            if (!up) {
                if (reconnect()) {
                    delay = kRetryMinSeconds;
                } else if (!stopping) {
                    std::lock_guard<std::mutex> lock(mutex);
                    std::fprintf(stderr, "[remote] %s; retrying in %d s\n",
                                 last_failure.c_str(), delay);
                    wait = delay;
                    delay = std::min(delay * 2, kRetryMaxSeconds);
                }
            }
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait_for(lock, std::chrono::seconds(wait), [this] { return stopping.load(); });
        }
    }

    // Send the segment and wait for its text. Caller holds mutex.
    bool request_locked(std::string& out) {
        uint8_t type;
        std::string payload;
        if (!send_frame(link.fd, kAudio, segment.data(), segment.size() * 2) ||
            !recv_frame(link.fd, type, payload, kReplyMs)) {
            std::string why = link.child_stderr();
            link.close_down();
            last_failure = host + " link lost" + (why.empty() ? "" : ": " + why);
            return false;
        }
        if (type == kText) { out = payload; return true; }
        out.clear();
        error = host + ": " + (type == kError ? payload : std::string("unexpected reply"));
        return true;   // the link is fine; the request failed (error set)
    }
};

extern "C" {

nerd_dictation_t nerd_dictation_create(void) {
    const char* host = std::getenv("NERD_DICTATION_REMOTE_HOST");
    if (!host || !*host) {
        std::fprintf(stderr, "[remote] NERD_DICTATION_REMOTE_HOST is not set\n");
        return nullptr;
    }
    auto* b = new nerd_dictation_backend();
    b->host = host;
    b->keeper = std::thread([b] { b->keep(); });
    return b;
}

void nerd_dictation_destroy(nerd_dictation_t b) {
    if (!b) return;
    b->stopping = true;
    b->wake.notify_all();
    // A handshake in progress ends when its child does.
    pid_t connecting = b->connecting_pid;
    if (connecting > 0) ::kill(connecting, SIGTERM);
    if (b->keeper.joinable()) b->keeper.join();
    {
        std::lock_guard<std::mutex> lock(b->mutex);
        b->link.close_down();
    }
    delete b;
}

int nerd_dictation_get_info(nerd_dictation_t b, struct nerd_dictation_info* info) {
    if (!b || !info || info->struct_size < sizeof(*info)) return -1;
    info->name = "remote";
    info->streaming = 0;
    info->sample_rate = kSampleRate;
    info->channels = 1;
    info->bits_per_sample = 16;
    return 0;
}

int nerd_dictation_feed(nerd_dictation_t b, const int16_t* samples, size_t n) {
    if (!b || (!samples && n)) return -1;
    if (b->segment.size() + n > kMaxSegmentSamples) {
        b->error = "segment longer than 30 s";
        return -2;
    }
    b->segment.insert(b->segment.end(), samples, samples + n);
    return 0;
}

int nerd_dictation_result(nerd_dictation_t b, struct nerd_dictation_result* r) {
    if (!b || !r) return -1;
    r->text = "";
    r->is_final = 0;
    r->confidence = -1.0f;
    return 0;
}

int nerd_dictation_finalize(nerd_dictation_t b, struct nerd_dictation_result* r) {
    if (!b || !r) return -1;
    r->text = "";
    r->is_final = 1;
    r->confidence = -1.0f;
    b->text.clear();
    b->error.clear();
    if (b->segment.empty()) return 0;

    std::unique_lock<std::mutex> lock(b->mutex);
    bool ok = false;
    if (b->link.alive()) {
        ok = b->request_locked(b->text);
        if (!ok) {
            // A link that died since the last segment (sleep, network
            // change): one immediate reconnect and retry, then give up.
            lock.unlock();
            bool fresh = b->reconnect();
            lock.lock();
            if (fresh) ok = b->request_locked(b->text);
        }
    }
    if (!ok && b->error.empty()) {
        b->error = (b->last_failure.empty() ? b->host + " not connected yet"
                                            : b->last_failure)
                   + " - nothing typed";
    }
    lock.unlock();
    b->wake.notify_all();
    b->segment.clear();
    if (!b->error.empty()) {
        std::fprintf(stderr, "[remote] %s\n", b->error.c_str());
        return -3;
    }
    r->text = b->text.c_str();
    return 0;
}

int nerd_dictation_reset(nerd_dictation_t b) {
    if (!b) return -1;
    b->segment.clear();
    return 0;
}

int nerd_dictation_transcribe_stream(nerd_dictation_t b,
                                     nerd_dictation_audio_callback audio_cb,
                                     nerd_dictation_result_callback result_cb,
                                     void* user_data) {
    if (!b || !audio_cb) return -1;
    std::vector<int16_t> chunk(kSampleRate / 10);
    for (;;) {
        int got = audio_cb(chunk.data(), chunk.size(), user_data);
        if (got < 0) { b->error = "audio callback failed"; return -2; }
        if (got == 0) break;
        int rc = nerd_dictation_feed(b, chunk.data(), static_cast<size_t>(got));
        if (rc < 0) return rc;
    }
    struct nerd_dictation_result r;
    int rc = nerd_dictation_finalize(b, &r);
    if (rc == 0 && result_cb) result_cb(&r, user_data);
    return rc;
}

const char* nerd_dictation_get_error(nerd_dictation_t b) {
    return b && !b->error.empty() ? b->error.c_str() : nullptr;
}

int nerd_dictation_api_version(void) { return NERD_DICTATION_API_VERSION; }

}  // extern "C"
