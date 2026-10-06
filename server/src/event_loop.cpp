#include "baton/event_loop.h"

// Headers you will likely need: <sys/epoll.h>, <sys/socket.h>, <netinet/in.h>, <arpa/inet.h>,
// <fcntl.h> (O_NONBLOCK), <unistd.h>, <cerrno>.
#include "baton/todo.h"

namespace baton {

Server::Server(ServerConfig config, TaskStore& store, WalWriter* wal, const Clock& clock)
    : config_(std::move(config)), store_(store), wal_(wal), clock_(clock) {}

Server::~Server() {
    // TODO(richard): close every connection fd, the listening socket and the epoll fd.
}

void Server::run() { todo("Server::run"); }

}  // namespace baton
