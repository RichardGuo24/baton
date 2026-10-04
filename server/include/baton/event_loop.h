#pragma once

// Single-threaded epoll server. One loop iteration:
//   1. epoll_wait(timeout = time until store.next_expiry(), or -1 if none)
//   2. accept new connections; read available bytes into each connection's in_buf
//   3. for every complete line: parse -> prepare -> wal.append (mutations) or answer directly
//      (list, heartbeat, errors)
//   4. collect_expired(now) -> wal.append each ExpireM
//   5. wal.sync()   <- one fsync covers everything appended this iteration (group commit)
//   6. apply every pending mutation, queue its response into out_buf
//   7. write out_bufs (handle partial writes; watch EPOLLOUT when a socket is full)
// Replies to step-3 mutations are only queued after step 5, so "ok" always means "on disk".

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
};

class Server {
   public:
    Server(ServerConfig config, TaskStore& store, WalWriter& wal, const Clock& clock);
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
    WalWriter& wal_;
    const Clock& clock_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    bool running_ = false;
    std::unordered_map<int, Connection> conns_;
};

}  // namespace baton
