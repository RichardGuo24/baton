#include "baton/mutation.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace baton {

using nlohmann::json;

namespace {
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
}  // namespace

Record to_record(const Mutation& m) {
    return std::visit(
        overloaded{
            [](const CreateM& x) {
                return Record{RecordType::Create,
                              json{{"id", x.id}, {"title", x.title}, {"description", x.description}, {"ts", x.ts}}.dump()};
            },
            [](const ClaimM& x) {
                return Record{RecordType::Claim,
                              json{{"task", x.task}, {"agent", x.agent}, {"paths", x.paths}, {"expiry", x.expiry}, {"ts", x.ts}}.dump()};
            },
            [](const LockM& x) {
                return Record{RecordType::Lock, json{{"task", x.task}, {"paths", x.paths}}.dump()};
            },
            [](const NoteM& x) {
                return Record{RecordType::Note,
                              json{{"task", x.task}, {"agent", x.agent}, {"text", x.text}, {"ts", x.ts}}.dump()};
            },
            [](const CompleteM& x) {
                return Record{RecordType::Complete,
                              json{{"task", x.task}, {"agent", x.agent}, {"summary", x.summary}, {"ts", x.ts}}.dump()};
            },
            [](const ReleaseM& x) {
                return Record{RecordType::Release,
                              json{{"task", x.task}, {"agent", x.agent}, {"reason", x.reason}, {"ts", x.ts}}.dump()};
            },
            [](const ExpireM& x) {
                return Record{RecordType::Expire, json{{"task", x.task}, {"lease_gen", x.lease_gen}, {"ts", x.ts}}.dump()};
            },
        },
        m);
}

Mutation from_record(const Record& r) {
    json j = json::parse(r.payload, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error("WAL record payload is not a JSON object");
    try {
        switch (r.type) {
            case RecordType::Create:
                return CreateM{j.at("id"), j.at("title"), j.at("description"), j.at("ts")};
            case RecordType::Claim:
                return ClaimM{j.at("task"), j.at("agent"), j.at("paths").get<std::vector<std::string>>(),
                              j.at("expiry"), j.at("ts")};
            case RecordType::Lock:
                return LockM{j.at("task"), j.at("paths").get<std::vector<std::string>>()};
            case RecordType::Note:
                return NoteM{j.at("task"), j.at("agent"), j.at("text"), j.at("ts")};
            case RecordType::Complete:
                return CompleteM{j.at("task"), j.at("agent"), j.at("summary"), j.at("ts")};
            case RecordType::Release:
                return ReleaseM{j.at("task"), j.at("agent"), j.at("reason"), j.at("ts")};
            case RecordType::Expire:
                return ExpireM{j.at("task"), j.at("lease_gen"), j.at("ts")};
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("bad WAL record payload: ") + e.what());
    }
    throw std::runtime_error("unknown WAL record type " + std::to_string(static_cast<int>(r.type)));
}

}  // namespace baton
