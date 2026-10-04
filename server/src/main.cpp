// baton-server entry point: parse flags, replay the WAL, then serve forever.

#include <cstdlib>
#include <iostream>
#include <string>

#include "baton/clock.h"
#include "baton/event_loop.h"
#include "baton/mutation.h"
#include "baton/task_store.h"
#include "baton/wal.h"

namespace {

void usage() {
    std::cerr << "usage: baton-server [--host ADDR] [--port N] [--wal PATH]\n"
                 "  defaults: --host 0.0.0.0 --port 7000 --wal baton.wal\n";
}

}  // namespace

int main(int argc, char** argv) {
    baton::ServerConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--host") config.host = next();
        else if (arg == "--port") config.port = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--wal") config.wal_path = next();
        else if (arg == "-h" || arg == "--help") { usage(); return 0; }
        else { usage(); return 2; }
    }

    try {
        baton::SystemClock clock;
        baton::TaskStore store;

        // 1. Rebuild state from the log through the same apply() the live server uses.
        baton::ReplayResult replay = baton::replay_wal(config.wal_path);
        for (const auto& record : replay.records) store.apply(baton::from_record(record));
        store.rearm_leases(clock.now());
        std::cerr << "replayed " << replay.records.size() << " records from " << config.wal_path
                  << (replay.truncated_tail ? " (truncated a torn tail)" : "") << "\n";

        // 2. Open the log for appending and start serving.
        baton::WalWriter wal(config.wal_path);
        baton::Server server(config, store, wal, clock);
        std::cerr << "listening on " << config.host << ":" << config.port << "\n";
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "baton-server: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
