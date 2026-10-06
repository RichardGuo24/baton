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
#include <unordered_map>

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

    // TODO(richard): create a non-blocking listening socket and the epoll instance, then loop
    // until stop() is called.
    void run();
    void stop() { running_ = false; }

   private:
    struct Connection {
        int fd = -1;
        std::string in_buf;   // bytes read but not yet a full line
        std::string out_buf;  // bytes queued but not yet written
    };

    ServerConfig config_;
    TaskStore& store_;
    WalWriter* wal_;  // null when running with --no-wal
    const Clock& clock_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    bool running_ = false;
    std::unordered_map<int, Connection> conns_;
};

}  // namespace baton
