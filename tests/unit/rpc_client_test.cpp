/// \file
/// Tests for the client's byte-level half, and for the one property that needs both halves.
///
/// `rpc/client.hpp` and `rpc/http.hpp` are two descriptions of the same bytes. Every test in
/// `rpc_http_screen_test.cpp` asks whether the server refuses what it should; the tests here ask
/// the complementary question, which no test of either side alone can answer: whether what this
/// client sends is what this server accepts, and whether what this server sends is what this
/// client can read.
///
/// That question matters more than it looks. The server's checks are deliberately strict — a
/// `Host` naming the wrong port, a missing `Content-Type` and a doubled `Content-Length` are all
/// refusals — and each one is a rule the client has to satisfy from the other direction. A test
/// that only screened hand-written requests would keep passing if `RenderCall` stopped emitting
/// a header, and the symptom would be an operator whose CLI is refused by their own node.
///
/// `ParseArgument` is here for a different reason. It is the one function in either half whose
/// mistakes are silent: a wrong refusal is visible, but sending `getblockhash "0"` where the
/// operator typed `getblockhash 0` produces a plausible error about the argument's shape, and
/// nothing anywhere reports that the call was not the one that was typed.

#include <amarian/rpc/client.hpp>

#include <amarian/chain/block_index.hpp>
#include <amarian/chain/block_store.hpp>
#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/mempool.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/rpc/http.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/coins.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace amarian::rpc {
namespace {

using nlohmann::json;

constexpr uint8_t FUTURE_LOCK_VERSION = 200;
constexpr int64_t NOW = REGTEST_PARAMS.genesis_timestamp + 600;
constexpr int64_t REQUEST_ID = 1;
/// The port the client renders and the server checks. Regtest's, because that is the pair an
/// operator actually runs.
constexpr uint16_t PORT = REGTEST_PARAMS.default_rpc_port;

/// The same shape of node the dispatch tests use: an in-memory chain seeded with regtest
/// genesis. Enough for `AnswerJsonRpc` to produce a real reply, which is all these tests need
/// from it — what the methods answer is `rpc_test.cpp`'s subject, not this file's.
struct NodeFixture {
    utxo::EmptyCoinsView base;
    chain::BlockIndex index = chain::BlockIndex::ForNetwork(REGTEST_PARAMS);
    utxo::CoinsCache coins = utxo::CoinsCache::Over(base);
    chain::MemoryBlockStore store;
    mempool::Mempool pool;
    chain::ChainState state{index, coins, store, REGTEST_PARAMS};
    Node node;

    NodeFixture() {
        node.state = &state;
        node.pool = &pool;
        node.params = &REGTEST_PARAMS;
        node.payout = Lock{.version = FUTURE_LOCK_VERSION, .program = ByteVec(4, 0x11)};
    }
};

/// A credential in the format the cookie produces, without touching a filesystem. `Cookie` is
/// exercised where it belongs — writing a file with the right permissions is not a property of
/// the wire format — and what these tests need is only that both halves agree on the header.
[[nodiscard]] std::string TestAuthorization() {
    return "Basic " + Base64Encode("__cookie__:0123456789abcdef");
}

// --- the two halves against each other --------------------------------------

TEST(RpcClient, WhatTheClientSendsIsWhatTheServerFrames) {
    const std::string body = RenderCallBody("getblockchaininfo", json::array(), REQUEST_ID);
    const std::string raw = RenderCall(TestAuthorization(), "127.0.0.1", PORT, body);

    const Framed framed = FrameRequest(raw);
    ASSERT_EQ(framed.state, Framing::Complete) << framed.request.body;
    EXPECT_EQ(framed.request.method, "POST");
    EXPECT_EQ(framed.request.target, "/");
    EXPECT_EQ(framed.request.body, body);
    // Nothing left over: a client that announced a length disagreeing with what it sent would
    // leave the server holding a fragment of the next request that will never come.
    EXPECT_EQ(framed.consumed, raw.size());
}

TEST(RpcClient, WhatTheClientSendsPassesEveryScreeningRule) {
    // The whole point of the file. `Screen` refuses a request over the `Host` header, the
    // credential, the method, the target and the content type, and each refusal is a header
    // `RenderCall` has to get right. Asserting the composite here means dropping any one of
    // them fails a test instead of failing an operator.
    AuthPolicy policy;
    policy.expected_authorization = TestAuthorization();
    policy.port = PORT;

    const std::string body = RenderCallBody("getmininginfo", json::array(), REQUEST_ID);
    const Framed framed =
        FrameRequest(RenderCall(policy.expected_authorization, "127.0.0.1", PORT, body));
    ASSERT_EQ(framed.state, Framing::Complete);

    const std::optional<HttpStatus> refusal = Screen(framed.request, policy);
    EXPECT_FALSE(refusal.has_value())
        << "the client's own request was refused with "
        << static_cast<int>(refusal.value_or(HttpStatus::Ok));
}

TEST(RpcClient, TheHostTheClientRendersIsCheckedAgainstThePortItNames) {
    // The other side of the same agreement: the `Host` header is not decoration, and a client
    // rendering a port it is not talking to is refused. This is what makes the rebinding guard
    // meaningful rather than a header both ends copy blindly.
    AuthPolicy policy;
    policy.expected_authorization = TestAuthorization();
    policy.port = PORT;

    const std::string body = RenderCallBody("getmempoolinfo", json::array(), REQUEST_ID);
    const Framed framed = FrameRequest(
        RenderCall(policy.expected_authorization, "127.0.0.1", PORT + 1, body));
    ASSERT_EQ(framed.state, Framing::Complete);

    const std::optional<HttpStatus> refusal = Screen(framed.request, policy);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_EQ(*refusal, HttpStatus::Forbidden);
}

TEST(RpcClient, WhatTheServerRendersIsWhatTheClientReads) {
    NodeFixture fixture;
    const std::string body = RenderCallBody("getblockchaininfo", json::array(), REQUEST_ID);
    const RpcAnswer answer = AnswerJsonRpc(fixture.node, body, NOW);
    const std::string raw = RenderResponse(answer.status, answer.body);

    const ParsedResponse parsed = ParseResponse(raw);
    ASSERT_EQ(parsed.state, ResponseState::Complete) << parsed.failure;
    EXPECT_EQ(parsed.status, static_cast<int>(HttpStatus::Ok));

    const json envelope = json::parse(parsed.body, nullptr, false);
    ASSERT_FALSE(envelope.is_discarded());
    // The id the client sent, back untouched. The CLI refuses to print a result whose id does
    // not match, so a server that dropped the field would make every call look like an answer
    // from something else.
    ASSERT_TRUE(envelope.contains("id"));
    EXPECT_EQ(envelope.at("id").get<int64_t>(), REQUEST_ID);
    ASSERT_TRUE(envelope.contains("result"));
    EXPECT_TRUE(envelope.at("result").contains("chain"));
}

TEST(RpcClient, AnErrorEnvelopeCarriesTheSpecificReasonInData) {
    // Why the CLI prints `data` and not `message`. The node computed a specific reason; the
    // envelope's `message` is the class it belongs to. If these two ever became the same
    // string this test would still pass, but if `data` stopped being populated the CLI would
    // silently fall back to the vaguer half, and this is what catches that.
    NodeFixture fixture;
    const std::string body = RenderCallBody("getblockhash", json::array({99U}), REQUEST_ID);
    const RpcAnswer answer = AnswerJsonRpc(fixture.node, body, NOW);

    const ParsedResponse parsed = ParseResponse(RenderResponse(answer.status, answer.body));
    ASSERT_EQ(parsed.state, ResponseState::Complete) << parsed.failure;

    const json envelope = json::parse(parsed.body, nullptr, false);
    ASSERT_FALSE(envelope.is_discarded());
    ASSERT_TRUE(envelope.contains("error"));
    const json& error = envelope.at("error");
    ASSERT_FALSE(error.is_null());
    ASSERT_TRUE(error.contains("data")) << error.dump();
    EXPECT_FALSE(error.at("data").get<std::string>().empty());
    EXPECT_NE(error.at("data").get<std::string>(), error.at("message").get<std::string>());
}

TEST(RpcClient, ARefusalIsReadableEvenThoughItIsNotJsonRpc) {
    // A request turned away before dispatch gets plain text, and the client still has to be
    // able to report the status: this is the path an operator hits with a stale cookie, and
    // "could not parse the reply" would be the wrong thing to tell them.
    const ParsedResponse parsed = ParseResponse(RenderRefusal(HttpStatus::Unauthorized));
    ASSERT_EQ(parsed.state, ResponseState::Complete) << parsed.failure;
    EXPECT_EQ(parsed.status, static_cast<int>(HttpStatus::Unauthorized));
    EXPECT_FALSE(parsed.body.empty());
    EXPECT_TRUE(json::parse(parsed.body, nullptr, false).is_discarded());
}

// --- reply framing ----------------------------------------------------------

TEST(RpcClient, ParseResponseNeedsMoreForAPrefix) {
    const std::string raw = RenderResponse(HttpStatus::Ok, R"({"result":1,"id":1})");
    for (const size_t cut : {size_t{1}, raw.find("\r\n\r\n"), raw.size() - 1}) {
        const ParsedResponse parsed = ParseResponse(std::string_view(raw).substr(0, cut));
        EXPECT_EQ(parsed.state, ResponseState::NeedMore) << "cut at " << cut;
    }
    EXPECT_EQ(ParseResponse(raw).state, ResponseState::Complete);
}

TEST(RpcClient, FinishResponseTurnsAShortBodyIntoAFailureRatherThanAWait) {
    // The same buffer, and the socket is the only thing that distinguishes the two answers.
    // Without `FinishResponse` a truncated reply would be waited on forever.
    const std::string raw = RenderResponse(HttpStatus::Ok, R"({"result":1,"id":1})");
    const std::string_view truncated = std::string_view(raw).substr(0, raw.size() - 5);

    EXPECT_EQ(ParseResponse(truncated).state, ResponseState::NeedMore);
    const ParsedResponse finished = FinishResponse(truncated);
    EXPECT_EQ(finished.state, ResponseState::Malformed);
    EXPECT_FALSE(finished.failure.empty());
}

TEST(RpcClient, FinishResponseCompletesAReplyWithNoAnnouncedLength) {
    // Legitimate HTTP/1.1: no `Content-Length`, body delimited by the close. This server never
    // sends one, and the client reads it anyway rather than requiring a header it happens to
    // always get.
    const std::string raw = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"result\":1}";
    EXPECT_EQ(ParseResponse(raw).state, ResponseState::NeedMore);
    const ParsedResponse finished = FinishResponse(raw);
    ASSERT_EQ(finished.state, ResponseState::Complete) << finished.failure;
    EXPECT_EQ(finished.status, 200);
    EXPECT_EQ(finished.body, "{\"result\":1}");
}

TEST(RpcClient, FinishResponseReportsACloseWithNoReplyAtAll) {
    const ParsedResponse finished = FinishResponse("");
    EXPECT_EQ(finished.state, ResponseState::Malformed);
    EXPECT_FALSE(finished.failure.empty());
}

TEST(RpcClient, ParseResponseRefusesADoubledContentLength) {
    // Refused, not resolved, for the reason the server refuses the same thing: a body two
    // readers would delimit differently is not a body worth reading.
    const std::string raw =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}";
    const ParsedResponse parsed = ParseResponse(raw);
    EXPECT_EQ(parsed.state, ResponseState::Malformed);
    EXPECT_FALSE(parsed.failure.empty());
}

TEST(RpcClient, ParseResponseRefusesRepliesThatAreNotHttp) {
    for (const std::string_view raw :
         {"garbage\r\n\r\n", "HTTP/1.1\r\n\r\n", "HTTP/1.1 abc OK\r\n\r\n",
          "HTTP/1.1 99 Too Small\r\n\r\n"}) {
        const ParsedResponse parsed = ParseResponse(raw);
        EXPECT_EQ(parsed.state, ResponseState::Malformed) << raw;
        EXPECT_FALSE(parsed.failure.empty()) << raw;
    }
}

TEST(RpcClient, ParseResponseRefusesALengthLargerThanItWillHold) {
    const std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n";
    const ParsedResponse parsed = ParseResponse(raw);
    EXPECT_EQ(parsed.state, ResponseState::Malformed);
}

TEST(RpcClient, ParseResponseIgnoresTrailingBytesPastTheAnnouncedLength) {
    const std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}EXTRA";
    const ParsedResponse parsed = ParseResponse(raw);
    ASSERT_EQ(parsed.state, ResponseState::Complete) << parsed.failure;
    EXPECT_EQ(parsed.body, "{}");
}

// --- the argument rule ------------------------------------------------------

TEST(RpcClient, ParseArgumentReadsJsonAsJson) {
    EXPECT_EQ(ParseArgument("0"), json(0));
    EXPECT_EQ(ParseArgument("42"), json(42));
    EXPECT_EQ(ParseArgument("true"), json(true));
    EXPECT_EQ(ParseArgument("[]"), json::array());
    EXPECT_EQ(ParseArgument(R"({"a":1})"), json::object({{"a", 1}}));
    EXPECT_EQ(ParseArgument(R"("quoted")"), json("quoted"));
}

TEST(RpcClient, ParseArgumentReadsAnythingElseAsAString) {
    EXPECT_EQ(ParseArgument("deadbeef"), json("deadbeef"));
    EXPECT_EQ(ParseArgument(""), json(""));
    EXPECT_EQ(ParseArgument("not json at all"), json("not json at all"));
    EXPECT_EQ(ParseArgument("0x10"), json("0x10"));
}

TEST(RpcClient, ParseArgumentSendsALongDigitRunAsBytesNotANumber) {
    // The guard that matters. A serialised block is hex, and one in sixteen million happens to
    // contain no letters; read as a number it would be refused for the wrong reason, and the
    // operator would be told their block was not hex when the client had never sent it as hex.
    const std::string block_shaped(184, '4');
    const json parsed = ParseArgument(block_shaped);
    ASSERT_TRUE(parsed.is_string());
    EXPECT_EQ(parsed.get<std::string>(), block_shaped);

    // The boundary: twenty digits is every value a uint64 can hold, so it stays a number.
    EXPECT_TRUE(ParseArgument("12345678901234567890").is_number());
    EXPECT_TRUE(ParseArgument("123456789012345678901").is_string());
}

TEST(RpcClient, ParseArgumentQuotingStillForcesAStringAtAnyLength) {
    EXPECT_EQ(ParseArgument(R"("7")"), json("7"));
}

// --- the request body -------------------------------------------------------

TEST(RpcClient, RenderCallBodyIsAJsonRpcRequestTheServerAccepts) {
    const json body =
        json::parse(RenderCallBody("getblockhash", json::array({0U}), REQUEST_ID), nullptr, false);
    ASSERT_FALSE(body.is_discarded());
    EXPECT_EQ(body.at("jsonrpc").get<std::string>(), "2.0");
    EXPECT_EQ(body.at("method").get<std::string>(), "getblockhash");
    EXPECT_EQ(body.at("id").get<int64_t>(), REQUEST_ID);
    EXPECT_EQ(body.at("params"), json::array({0U}));
}

}  // namespace
}  // namespace amarian::rpc
