#pragma once

// Single-threaded epoll server. One loop iteration:
//   1. epoll_wait(timeout = time until store.next_expiry(), or -1 if none)
//   2. accept new connections; read available bytes into each connection's in_buf
//   3. for every complete line, in arrival order: parse -> prepare -> apply -> wal.append.
//      The reply (ok, error, or a read like list) is HELD, not sent.
//   4. collect_expired(now) -> apply + wal.append each ExpireM
//   5. wal.sync()   <- one fsync covers everything appended this iteration (group commit)
//   6. move every held reply into its connection's out_buf, in arrival order
//   7. write out_bufs (handle partial writes; watch EPOLLOUT when a socket is full)
//
// Why apply in step 3 instead of after the fsync: if two claims for the same task arrive in one
// iteration, the second must see the first one's effect, or both would succeed. Holding every
// reply until step 5 keeps "ok" meaning "on disk", and nobody ever sees non-durable state.
// If sync() fails, memory is ahead of disk and cannot be rolled back, so the server exits.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "baton/clock.h"
#include "baton/task_store.h"
#include "baton/wal.h"

namespace baton {

struct ServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 7000;
    std::string wal_path = "baton.wal";
    bool use_wal = true;  // --no-wal: keep all state in memory only (lost on restart)
};

class Server {
   public:
    // `wal` may be null (--no-wal). The loop still runs the same steps; it just skips the disk.
    Server(ServerConfig config, TaskStore& store, WalWriter* wal, const Clock& clock);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Bind the listening socket, then serve until SIGINT/SIGTERM or stop(). Throws on setup
    // failure, and if a WAL sync fails (memory is then ahead of disk, so we must not continue).
    void run();
    void stop() { running_ = false; }

    // Largest request line we accept. A client that sends more without a newline gets an error
    // and is disconnected, so one bad client cannot make the server buffer unbounded memory.
    static constexpr size_t kMaxLineBytes = 1 << 20;  // 1 MiB
    // If a client stops reading replies, stop reading its requests once this much output is queued.
    static constexpr size_t kMaxOutBytes = 16 << 20;  // 16 MiB

   private:
    struct Connection {
        int fd = -1;
        std::string in_buf;        // bytes read but not yet a full line
        std::string out_buf;       // bytes queued but not yet written
        bool stop_reading = false;  // peer sent EOF, or we gave up on its input
        bool closing = false;       // queued to be closed at the end of this iteration
        uint32_t events = 0;        // what this fd is currently registered for in epoll
    };

    // A reply waiting for the iteration's commit point (see the comment at the top of this file).
    struct HeldReply {
        int fd;
        std::string line;
    };

    void open_listener();
    void open_signalfd();
    void accept_all();
    void on_readable(Connection& c);
    void handle_line(Connection& c, std::string_view line);
    void commit_and_release_replies();
    void flush(Connection& c);
    void update_interest(Connection& c);
    void mark_for_close(Connection& c);
    void close_marked();

    ServerConfig config_;
    TaskStore& store_;
    WalWriter* wal_;  // null when running with --no-wal
    const Clock& clock_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    int signal_fd_ = -1;
    bool running_ = false;
    std::unordered_map<int, Connection> conns_;
    std::vector<HeldReply> held_;  // replies produced this iteration, in arrival order
    std::vector<int> to_close_;    // fds to close once this iteration's events are handled
};

}  // namespace baton
