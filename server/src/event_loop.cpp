#include "baton/event_loop.h"

#include <arpa/inet.h>    // inet_pton
#include <fcntl.h>        // O_NONBLOCK
#include <netinet/in.h>   // sockaddr_in
#include <netinet/tcp.h>  // TCP_NODELAY
#include <signal.h>       // sigset_t, sigprocmask
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>  // read, close

#include <cerrno>
#include <cstring>  // strerror
#include <iostream>
#include <stdexcept>

#include "baton/mutation.h"
#include "baton/protocol.h"

namespace baton {

namespace {

// Turn a failed syscall into an exception with the reason in it. Only used during setup; once the
// loop is running, per-connection errors close that connection instead of stopping the server.
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

}  // namespace

Server::Server(ServerConfig config, TaskStore& store, WalWriter* wal, const Clock& clock)
    : config_(std::move(config)), store_(store), wal_(wal), clock_(clock) {}

Server::~Server() {
    // Every fd we opened is ours to close. Closing an fd also removes it from the epoll set.
    for (auto& [fd, _] : conns_) ::close(fd);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (signal_fd_ >= 0) ::close(signal_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void Server::open_listener() {
    // SOCK_NONBLOCK: accept() must never block the loop. With a blocking listener, a client that
    // connects and resets before we call accept() could leave us stuck waiting for the next one.
    // SOCK_CLOEXEC: don't leak the fd into child processes (good hygiene; we never exec today).
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) fail("socket");

    // SO_REUSEADDR lets us bind again right after a restart. Without it, connections from the
    // previous run that are still in TIME_WAIT make bind() fail with EADDRINUSE for ~60 s, which
    // is exactly when you want the server back (crash tests restart it constantly).
    int one = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) fail("setsockopt");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);  // network byte order (big-endian)
    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("--host must be an IPv4 address like 127.0.0.1, got '" +
                                 config_.host + "'");
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        fail("bind " + config_.host + ":" + std::to_string(config_.port));
    }
    // The backlog is how many finished handshakes the kernel queues for us before we accept().
    if (::listen(listen_fd_, SOMAXCONN) < 0) fail("listen");

    // With --port 0 the kernel picks a free port; ask which one so we can print it (tests use this).
    socklen_t len = sizeof addr;
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) fail("getsockname");
    config_.port = ntohs(addr.sin_port);
}

void Server::open_signalfd() {
    // Turn SIGINT/SIGTERM into something epoll can wait on. The usual alternative, a signal
    // handler, runs at an arbitrary point in the middle of our code and may only touch
    // async-signal-safe things. With signalfd the signal just becomes a readable fd, handled at a
    // normal point in the loop like any other event.
    // The signals must be blocked first, or the default action (terminate) still happens.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) fail("sigprocmask");
    signal_fd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ < 0) fail("signalfd");
}

// ---------------------------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------------------------

void Server::run() {
    open_listener();
    open_signalfd();

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) fail("epoll_create1");

    // We use epoll in its default LEVEL-triggered mode: epoll_wait keeps reporting an fd for as
    // long as it is readable (or writable), not just once when it becomes so. That means we may
    // read only part of what is available and simply get told again next iteration, instead of
    // being forced to drain every socket until EAGAIN (edge-triggered), where forgetting to drain
    // one socket silently stalls that client forever.
    for (int fd : {listen_fd_, signal_fd_}) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) fail("epoll_ctl add");
    }

    std::cerr << "listening on " << config_.host << ":" << config_.port
              << (wal_ ? "" : " (--no-wal: state is in memory only)") << std::endl;

    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];
    running_ = true;
    while (running_) {
        // Step 1. Wait for something to happen.
        // TODO(week 3): timeout = time until store_.next_expiry(), so the loop wakes up to expire
        // leases even when no client sends anything. Until then leases never expire: wait forever.
        int timeout_ms = -1;
        int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal (e.g. a debugger); just retry
            fail("epoll_wait");
        }

        // Steps 2 and 3. Accept, read, and handle every complete line. Replies are held.
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listen_fd_) {
                accept_all();
                continue;
            }
            if (fd == signal_fd_) {
                signalfd_siginfo info;
                if (::read(signal_fd_, &info, sizeof info) == sizeof info) {
                    std::cerr << "got signal " << info.ssi_signo << ", shutting down" << std::endl;
                }
                running_ = false;  // finish this iteration (commit + reply), then leave the loop
                continue;
            }

            auto it = conns_.find(fd);
            if (it == conns_.end()) continue;
            Connection& c = it->second;
            if (c.closing) continue;  // already given up on this one earlier in the iteration

            // EPOLLHUP/EPOLLERR are always reported, even if we did not ask for them. Reading is
            // the simplest way to find out what happened: read() returns 0 (EOF) or an error.
            if ((ev & (EPOLLIN | EPOLLHUP | EPOLLERR)) && !c.stop_reading) on_readable(c);
            if ((ev & (EPOLLOUT | EPOLLHUP | EPOLLERR)) && !c.closing) flush(c);
        }

        // Step 4. TODO(week 3): collect_expired(now), apply + append each ExpireM.

        // Steps 5-7. One fsync for everything appended above, then release the held replies.
        commit_and_release_replies();

        close_marked();
    }
}

// ---------------------------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------------------------

void Server::accept_all() {
    // Several clients may be waiting in the backlog. Accepting all of them now saves one
    // epoll_wait round trip per client (level-triggered would also just tell us again).
    while (true) {
        int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // backlog is empty
            if (errno == EINTR || errno == ECONNABORTED) continue;  // client gave up; next one
            // EMFILE/ENFILE (out of fds) and anything else: log and stop for this iteration.
            // Known gap: with level-triggered epoll the listener stays readable, so if we are out
            // of fds we will spin here until one frees up. Fine at this scale.
            std::cerr << "accept: " << std::strerror(errno) << std::endl;
            return;
        }

        // Replies are small and latency matters, so turn off Nagle's algorithm. Otherwise the
        // kernel may hold a small reply back waiting to batch it with more data, and combined with
        // the client's delayed ACK that can add ~40 ms to a request.
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        Connection& c = conns_[fd];
        c.fd = fd;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::cerr << "epoll_ctl add client: " << std::strerror(errno) << std::endl;
            ::close(fd);
            conns_.erase(fd);
            continue;
        }
        c.events = EPOLLIN;
    }
}

void Server::on_readable(Connection& c) {
    // One read() per wakeup, not "read until EAGAIN". Level-triggered epoll will report the
    // socket again next iteration if more is waiting, and reading a bounded amount per client per
    // iteration stops one fast client from starving everyone else.
    char buf[64 * 1024];
    ssize_t n = ::read(c.fd, buf, sizeof buf);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;  // try again later
        mark_for_close(c);  // ECONNRESET etc.: the client is gone, nobody to reply to
        return;
    }
    if (n == 0) {
        // EOF: the client will send no more requests. It may still be waiting for replies to the
        // ones it already sent (e.g. `printf '...\n' | nc`), so answer complete lines first and
        // close once our output is flushed. A partial line at EOF can never complete: drop it.
        c.stop_reading = true;
        c.in_buf.clear();
        update_interest(c);
        // Safe to close now if nothing is queued: we read at most once per connection per
        // iteration, so the read that saw EOF cannot have produced held replies for this client.
        // If output is still queued, flush() closes the connection once it drains.
        if (c.out_buf.empty()) mark_for_close(c);
        return;
    }

    // TCP is a byte stream, not a message stream: one read() can return half a request, or three
    // requests and the start of a fourth. Append to in_buf and only handle complete lines.
    c.in_buf.append(buf, static_cast<size_t>(n));

    size_t start = 0;
    while (true) {
        size_t nl = c.in_buf.find('\n', start);
        if (nl == std::string::npos) break;
        std::string_view line(c.in_buf.data() + start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);  // tolerate CRLF (telnet)
        if (!line.empty()) handle_line(c, line);
        start = nl + 1;
    }
    // Erase all handled lines at once. Erasing after each line would shift the rest of the
    // buffer every time: quadratic in the number of pipelined requests.
    c.in_buf.erase(0, start);

    if (c.in_buf.size() > kMaxLineBytes) {
        held_.push_back({c.fd, error_response(0, ErrorCode::BadRequest,
                                              "Request line is longer than 1 MiB without a "
                                              "newline. Send one JSON object per line.")});
        c.stop_reading = true;  // close after the error reply is flushed
        c.in_buf.clear();
        update_interest(c);
    }
}

void Server::handle_line(Connection& c, std::string_view line) {
    // No exception may escape into the loop: one bad request must never take the server down.
    // Every failure becomes an error reply to the client that sent it.
    std::string reply;
    uint64_t req_id = 0;
    try {
        Request req = parse_request(line);
        req_id = req.req_id;
        TimeMs now = clock_.now();

        if (req.op == Op::List) {
            reply = ok_response(req_id, store_.list(req.state_filter, now));
        } else if (req.op == Op::Heartbeat) {
            auto r = store_.heartbeat(req.agent, *req.task, now);
            if (auto* err = std::get_if<StoreError>(&r)) {
                reply = error_response(req_id, err->code, err->message, err->extra);
            } else {
                reply = ok_response(req_id, {{"lease_expiry", std::get<TimeMs>(r)}});
            }
        } else {
            PrepareResult p = store_.prepare(req, now);
            if (auto* err = std::get_if<StoreError>(&p)) {
                reply = error_response(req_id, err->code, err->message, err->extra);
            } else {
                const Mutation& m = std::get<Mutation>(p);
                // Log first, then change memory: the usual write-ahead order. The record is only
                // buffered here; the fsync happens once for the whole iteration. Applying now, not
                // after the fsync, is what lets the next request in this batch see this change.
                if (wal_) wal_->append(to_record(m));
                reply = ok_response(req_id, store_.apply(m));
            }
        }
    } catch (const ProtocolError& e) {
        reply = error_response(e.req_id(), ErrorCode::BadRequest,
                               std::string("Bad request: ") + e.what() +
                                   ". Fix the request and send it again.");
    } catch (const std::exception& e) {
        reply = error_response(req_id, ErrorCode::Internal,
                               std::string("Internal server error: ") + e.what() +
                                   ". Try again; if it keeps happening, report it.");
    }
    // Held, not written: nothing leaves the server until the commit point.
    held_.push_back({c.fd, std::move(reply)});
}

void Server::commit_and_release_replies() {
    if (held_.empty()) return;

    // Step 5: the group-commit point. One fsync makes every record appended this iteration
    // durable. If it throws, the exception leaves run(): memory already contains changes that
    // may not be on disk, and the only safe thing is to stop and let replay rebuild from the log.
    // The held replies are dropped, so no client is ever told "ok" about a lost change.
    if (wal_ && wal_->pending_bytes() > 0) wal_->sync();

    // Step 6: release replies in arrival order. Every reply, including reads and errors, waited
    // for the fsync, so a client can never see state that a crash could still take back, and
    // replies on one connection stay in the same order as its requests.
    for (HeldReply& r : held_) {
        auto it = conns_.find(r.fd);
        // Safe to look up by fd: closes are deferred to the end of the iteration (close_marked),
        // so an fd cannot be closed and reused by a new client between hold and release.
        if (it == conns_.end() || it->second.closing) continue;
        it->second.out_buf += r.line;
    }

    // Step 7: try to write each connection that got something. Most replies fit in the socket
    // buffer and go out right here, without waiting for an EPOLLOUT event.
    for (HeldReply& r : held_) {
        auto it = conns_.find(r.fd);
        if (it != conns_.end() && !it->second.closing && !it->second.out_buf.empty()) {
            flush(it->second);
        }
    }
    held_.clear();
}

void Server::flush(Connection& c) {
    while (!c.out_buf.empty()) {
        // send() may write only part of the buffer (a "short write") when the socket's send
        // buffer is nearly full, e.g. a slow client or a large list reply. Whatever was not
        // written stays in out_buf and we try again when epoll says the socket is writable.
        // MSG_NOSIGNAL: writing to a socket the peer closed raises SIGPIPE, whose default action
        // kills the whole process. With this flag we get an EPIPE error instead.
        ssize_t n = ::send(c.fd, c.out_buf.data(), c.out_buf.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // socket full: wait for EPOLLOUT
            mark_for_close(c);  // EPIPE, ECONNRESET: the client is gone
            return;
        }
        c.out_buf.erase(0, static_cast<size_t>(n));
    }
    update_interest(c);
    if (c.out_buf.empty() && c.stop_reading) mark_for_close(c);  // EOF'd client, all answered
}

void Server::update_interest(Connection& c) {
    // Ask epoll only for the events we can act on right now:
    //   EPOLLIN  unless we stopped reading this client, or its unread replies are piling up
    //            (backpressure: a client that never reads must not grow out_buf forever).
    //   EPOLLOUT only while out_buf has bytes. A socket is writable almost all the time, so with
    //            level-triggered epoll an always-on EPOLLOUT would wake us up constantly.
    uint32_t want = 0;
    if (!c.stop_reading && c.out_buf.size() < kMaxOutBytes) want |= EPOLLIN;
    if (!c.out_buf.empty()) want |= EPOLLOUT;
    if (want == c.events || c.closing) return;  // nothing changed: skip the syscall

    epoll_event ev{};
    ev.events = want;
    ev.data.fd = c.fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, c.fd, &ev) < 0) {
        mark_for_close(c);
        return;
    }
    c.events = want;
}

void Server::mark_for_close(Connection& c) {
    if (c.closing) return;
    c.closing = true;
    to_close_.push_back(c.fd);
}

void Server::close_marked() {
    // Closing happens here, after all of the iteration's events and replies are handled, rather
    // than the moment we notice a problem. The kernel reuses the lowest free fd number for the
    // next accept(), so if we closed fd 7 mid-iteration and accepted a new client as fd 7, a stale
    // event or held reply meant for the old client could land on the new one.
    for (int fd : to_close_) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);  // close() would also do this
        ::close(fd);
        conns_.erase(fd);
    }
    to_close_.clear();
}

}  // namespace baton
