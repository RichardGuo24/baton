#include <gtest/gtest.h>

#include "baton/protocol.h"

using namespace baton;
using nlohmann::json;

TEST(Protocol, ParsesClaimWithPaths) {
    Request r = parse_request(R"({"req_id":17,"op":"claim","agent":"agent-c","task":3,"paths":["api/predict/","models/loader.py"]})");
    EXPECT_EQ(r.req_id, 17u);
    EXPECT_EQ(r.op, Op::Claim);
    EXPECT_EQ(r.agent, "agent-c");
    ASSERT_TRUE(r.task.has_value());
    EXPECT_EQ(*r.task, 3u);
    EXPECT_EQ(r.paths, (std::vector<std::string>{"api/predict/", "models/loader.py"}));
}

TEST(Protocol, ClaimWithoutTaskMeansOldestOpen) {
    Request r = parse_request(R"({"req_id":1,"op":"claim","agent":"a"})");
    EXPECT_FALSE(r.task.has_value());
}

TEST(Protocol, CreateNeedsTitleButNoAgent) {
    Request r = parse_request(R"({"req_id":2,"op":"create","title":"Write tests"})");
    EXPECT_EQ(r.op, Op::Create);
    EXPECT_EQ(r.title, "Write tests");
    EXPECT_THROW(parse_request(R"({"req_id":2,"op":"create"})"), ProtocolError);
}

TEST(Protocol, ListAcceptsStateFilter) {
    Request r = parse_request(R"({"op":"list","state":"open"})");
    ASSERT_TRUE(r.state_filter.has_value());
    EXPECT_EQ(*r.state_filter, TaskState::Open);
    EXPECT_THROW(parse_request(R"({"op":"list","state":"bogus"})"), ProtocolError);
}

TEST(Protocol, RejectsMalformedInput) {
    EXPECT_THROW(parse_request("not json"), ProtocolError);
    EXPECT_THROW(parse_request("[1,2,3]"), ProtocolError);
    EXPECT_THROW(parse_request(R"({"op":"explode"})"), ProtocolError);
    EXPECT_THROW(parse_request(R"({"op":"note","agent":"a","task":1})"), ProtocolError);   // no text
    EXPECT_THROW(parse_request(R"({"op":"complete","agent":"a"})"), ProtocolError);        // no task
    EXPECT_THROW(parse_request(R"({"op":"lock","agent":"a","task":1,"paths":[]})"), ProtocolError);
}

TEST(Protocol, ErrorKeepsReqId) {
    try {
        parse_request(R"({"req_id":9,"op":"heartbeat"})");
        FAIL() << "expected ProtocolError";
    } catch (const ProtocolError& e) {
        EXPECT_EQ(e.req_id(), 9u);
    }
}

TEST(Protocol, ResponsesAreSingleJsonLines) {
    std::string ok = ok_response(4, json{{"task", 1}});
    ASSERT_EQ(ok.back(), '\n');
    json j = json::parse(ok);
    EXPECT_EQ(j["req_id"], 4);
    EXPECT_EQ(j["ok"], true);

    std::string err = error_response(5, ErrorCode::PathLocked, "locked", json{{"held_by", {{"task", 3}}}});
    json e = json::parse(err);
    EXPECT_EQ(e["ok"], false);
    EXPECT_EQ(e["code"], "PATH_LOCKED");
    EXPECT_EQ(e["held_by"]["task"], 3);
}
