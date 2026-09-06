/// \file
/// Tests for the HTTP framing, screening and rendering that guard the RPC interface.
///
/// `rpc::FrameRequest` and `rpc::Screen` are the two pure functions that decide whether a
/// byte string arriving on the RPC port is a request this server is willing to answer.
/// Every check that keeps a web page in the operator's browser from mining blocks is a
/// check in one of these two functions, and everything they refuse is something a
/// "simplification" could silently accept. So every rule below is tested with a request
/// that should pass it and one that should fail it, and the tests are the record of what
/// the rules are.

#include <amarian/rpc/http.hpp>

#include <amarian/util/types.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace amarian::rpc {
namespace {

// --- FrameRequest tests -----------------------------------------------------

TEST(RpcHttp, FrameRequestAcceptsAValidPost) {
    const std::string body = R"({"method":"getblocktemplate","params":[],"id":1})";
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Host: 127.0.0.1:12521\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: " +
                            std::to_string(body.size()) +
                            "\r\n"
                            "Authorization: Basic dGVzdDp0b2tlbg==\r\n"
                            "\r\n" +
                            body;

    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Complete);
    EXPECT_EQ(framed.request.method, "POST");
    EXPECT_EQ(framed.request.target, "/");
    EXPECT_EQ(framed.request.body, body);
    EXPECT_EQ(framed.consumed, raw.size());
}

TEST(RpcHttp, FrameRequestReturnsNeedMoreForPartialHeaders) {
    const std::string raw = "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::NeedMore);
}

TEST(RpcHttp, FrameRequestReturnsNeedMoreForPartialBody) {
    const std::string body = "{}";
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Content-Length: 5\r\n"
                            "\r\n"
                            "{}";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::NeedMore);
}

TEST(RpcHttp, FrameRequestRefusesHeaderSectionPastLimit) {
    std::string raw = "GET / HTTP/1.1\r\n";
    raw.append(MAX_HEADER_BYTES, 'X');
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::TooLarge);
    EXPECT_EQ(framed.status, HttpStatus::PayloadTooLarge);
}

TEST(RpcHttp, FrameRequestRefusesMalformedRequestLineMissingSpaces) {
    const std::string raw = "BADLINE\r\n\r\n";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
    EXPECT_EQ(framed.status, HttpStatus::BadRequest);
}

TEST(RpcHttp, FrameRequestRefusesUnsupportedHttpVersion) {
    const std::string raw = "POST / HTTP/2.0\r\n\r\n";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
    EXPECT_EQ(framed.status, HttpStatus::BadRequest);
}

TEST(RpcHttp, FrameRequestRefusesEmptyTarget) {
    const std::string raw = "POST  HTTP/1.1\r\n\r\n";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
}

TEST(RpcHttp, FrameRequestRefusesNonSlashTarget) {
    const std::string raw = "POST foo HTTP/1.1\r\n\r\n";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
}

TEST(RpcHttp, FrameRequestRefusesFoldedHeaderLines) {
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Host: 127.0.0.1\r\n"
                            " Content-Type: application/json\r\n"
                            "\r\n";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
}

TEST(RpcHttp, FrameRequestRefusesHeaderWithoutColon) {
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Host\r\n"
                            "\r\n";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
}

TEST(RpcHttp, FrameRequestRefusesHeaderNameWithSpaces) {
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Bad Name: value\r\n"
                            "\r\n";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
}

TEST(RpcHttp, FrameRequestRefusesTransferEncoding) {
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Content-Length: 2\r\n"
                            "Transfer-Encoding: chunked\r\n"
                            "\r\n"
                            "{}";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
}

TEST(RpcHttp, FrameRequestRefusesZeroContentLengthHeaders) {
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "\r\n"
                            "{}";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
}

TEST(RpcHttp, FrameRequestRefusesMultipleContentLengths) {
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Content-Length: 2\r\n"
                            "Content-Length: 3\r\n"
                            "\r\n"
                            "{}";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
}

TEST(RpcHttp, FrameRequestRefusesNonNumericContentLength) {
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Content-Length: abc\r\n"
                            "\r\n"
                            "{}";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Malformed);
}

TEST(RpcHttp, FrameRequestRefusesOversizedBody) {
    const std::string body(MAX_REQUEST_BYTES + 1, 'x');
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Content-Length: " +
                            std::to_string(body.size()) +
                            "\r\n"
                            "\r\n" +
                            body;
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::TooLarge);
    EXPECT_EQ(framed.status, HttpStatus::PayloadTooLarge);
}

TEST(RpcHttp, FrameRequestAcceptsMaximumSizedBody) {
    const std::string body(MAX_REQUEST_BYTES, 'x');
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Content-Length: " +
                            std::to_string(body.size()) +
                            "\r\n"
                            "\r\n" +
                            body;
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Complete);
    EXPECT_EQ(framed.request.body.size(), MAX_REQUEST_BYTES);
    EXPECT_EQ(framed.consumed, raw.size());
}

TEST(RpcHttp, FrameRequestTrimsHeaderValueWhitespace) {
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Content-Type:  application/json \t\r\n"
                            "Content-Length: 2\r\n"
                            "\r\n"
                            "{}";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Complete);
    const std::string* ct = framed.request.Header("Content-Type");
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(*ct, "application/json");
}

TEST(RpcHttp, FrameRequestConsumedExcludesTrailingData) {
    const std::string body = "{}";
    const std::string raw = "POST / HTTP/1.1\r\n"
                            "Content-Length: 2\r\n"
                            "\r\n" +
                            body + "EXTRA";
    const Framed framed = FrameRequest(raw);
    EXPECT_EQ(framed.state, Framing::Complete);
    EXPECT_EQ(framed.consumed, raw.size() - std::string_view("EXTRA").size());
}

// --- Screen tests -----------------------------------------------------------

/// A valid AuthPolicy: port 12521, with a credential set.
struct ScreenFixture {
    AuthPolicy policy;
    HttpRequest valid;

    ScreenFixture() {
        policy.expected_authorization = "Basic dmFsaWQ6dG9rZW4=";
        policy.port = 12521;
        valid.method = "POST";
        valid.target = "/";
        valid.headers = {{"Host", "127.0.0.1:12521"},
                         {"Content-Type", "application/json"},
                         {"Authorization", policy.expected_authorization},
                         {"Content-Length", "2"}};
        valid.body = "{}";
    }
};

TEST(RpcHttp, ScreenPassesAValidRequest) {
    ScreenFixture f;
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    EXPECT_FALSE(result.has_value()) << static_cast<int>(result.value_or(HttpStatus::Ok));
}

TEST(RpcHttp, ScreenRefusesMissingAuthorization) {
    ScreenFixture f;
    f.valid.headers.clear();
    f.valid.headers = {{"Host", "127.0.0.1:12521"},
                       {"Content-Type", "application/json"},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::Unauthorized);
}

TEST(RpcHttp, ScreenRefusesWrongAuthorization) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "127.0.0.1:12521"},
                       {"Content-Type", "application/json"},
                       {"Authorization", "Basic d3Jvbmc6dG9rZW4="},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::Unauthorized);
}

TEST(RpcHttp, ScreenRefusesMultipleAuthorizationHeaders) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "127.0.0.1:12521"},
                       {"Content-Type", "application/json"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::Unauthorized);
}

TEST(RpcHttp, ScreenRefusesMissingHost) {
    ScreenFixture f;
    f.valid.headers = {{"Content-Type", "application/json"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::Forbidden);
}

TEST(RpcHttp, ScreenRefusesMultipleHostHeaders) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "127.0.0.1"},
                       {"Host", "localhost"},
                       {"Content-Type", "application/json"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::Forbidden);
}

TEST(RpcHttp, ScreenRefusesNonLoopbackHost) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "evil.example.com:12521"},
                       {"Content-Type", "application/json"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::Forbidden);
}

TEST(RpcHttp, ScreenRefusesHostWithWrongPort) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "127.0.0.1:9999"},
                       {"Content-Type", "application/json"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::Forbidden);
}

TEST(RpcHttp, ScreenRefusesHostWithNonNumericPort) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "127.0.0.1:abc"},
                       {"Content-Type", "application/json"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::Forbidden);
}

TEST(RpcHttp, ScreenAcceptsLocalhost) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "localhost:12521"},
                       {"Content-Type", "application/json"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    EXPECT_FALSE(result.has_value());
}

TEST(RpcHttp, ScreenAcceptsIPv6Loopback) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "[::1]:12521"},
                       {"Content-Type", "application/json"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    EXPECT_FALSE(result.has_value());
}

TEST(RpcHttp, ScreenAcceptsHostWithoutPortWhenPortIsDefaultForLoopback) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "127.0.0.1"},
                       {"Content-Type", "application/json"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    EXPECT_FALSE(result.has_value());
}

TEST(RpcHttp, ScreenRefusesGetRequest) {
    ScreenFixture f;
    f.valid.method = "GET";
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::MethodNotAllowed);
}

TEST(RpcHttp, ScreenRefusesNonRootPath) {
    ScreenFixture f;
    f.valid.target = "/mine";
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::NotFound);
}

TEST(RpcHttp, ScreenRefusesMissingContentType) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "127.0.0.1:12521"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::UnsupportedMediaType);
}

TEST(RpcHttp, ScreenRefusesNonJsonContentType) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "127.0.0.1:12521"},
                       {"Content-Type", "application/x-www-form-urlencoded"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::UnsupportedMediaType);
}

TEST(RpcHttp, ScreenAcceptsContentTypeWithParameters) {
    ScreenFixture f;
    f.valid.headers = {{"Host", "127.0.0.1:12521"},
                       {"Content-Type", "application/json; charset=utf-8"},
                       {"Authorization", f.policy.expected_authorization},
                       {"Content-Length", "2"}};
    const std::optional<HttpStatus> result = Screen(f.valid, f.policy);
    EXPECT_FALSE(result.has_value());
}

TEST(RpcHttp, ScreenChecksAuthBeforeAnythingElse) {
    // A request that is wrong in multiple ways -- missing Host, wrong method -- must still
    // be refused as Unauthorised if the credential is wrong, because the caller should
    // learn nothing about what this server would have said about their request.
    ScreenFixture f;
    HttpRequest req;
    req.method = "GET";
    req.target = "/admin";
    req.headers = {{"Authorization", "Basic d3Jvbmc6dG9rZW4="}};
    req.body = "{}";
    const std::optional<HttpStatus> result = Screen(req, f.policy);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, HttpStatus::Unauthorized);
}

// --- ConstantTimeEquals tests -----------------------------------------------

TEST(RpcHttp, ConstantTimeEqualsAcceptsIdenticalStrings) {
    EXPECT_TRUE(ConstantTimeEquals("hello", "hello"));
}

TEST(RpcHttp, ConstantTimeEqualsRejectsDifferentStrings) {
    EXPECT_FALSE(ConstantTimeEquals("hello", "world"));
}

TEST(RpcHttp, ConstantTimeEqualsRejectsDifferentLengths) {
    EXPECT_FALSE(ConstantTimeEquals("hello", "helloo"));
}

TEST(RpcHttp, ConstantTimeEqualsAcceptsEmptyStrings) {
    EXPECT_TRUE(ConstantTimeEquals("", ""));
}

TEST(RpcHttp, ConstantTimeEqualsRejectsEmptyVsNonEmpty) {
    EXPECT_FALSE(ConstantTimeEquals("", "x"));
}

TEST(RpcHttp, ConstantTimeEqualsDetectsSingleByteDifference) {
    EXPECT_FALSE(ConstantTimeEquals("abcdef", "abcXef"));
}

// --- Base64Encode tests -----------------------------------------------------

TEST(RpcHttp, Base64EncodeEmpty) {
    EXPECT_EQ(Base64Encode(""), "");
}

TEST(RpcHttp, Base64EncodeKnownValues) {
    EXPECT_EQ(Base64Encode("f"), "Zg==");
    EXPECT_EQ(Base64Encode("fo"), "Zm8=");
    EXPECT_EQ(Base64Encode("foo"), "Zm9v");
    EXPECT_EQ(Base64Encode("foob"), "Zm9vYg==");
    EXPECT_EQ(Base64Encode("fooba"), "Zm9vYmE=");
    EXPECT_EQ(Base64Encode("foobar"), "Zm9vYmFy");
}

TEST(RpcHttp, Base64EncodeCredential) {
    const std::string encoded = Base64Encode("user:token");
    EXPECT_EQ(encoded, "dXNlcjp0b2tlbg==");
}

// --- RenderResponse / RenderRefusal tests -----------------------------------

TEST(RpcHttp, RenderResponseIncludesStatusLine) {
    const std::string response = RenderResponse(HttpStatus::Ok, "{\"ok\":true}");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST(RpcHttp, RenderResponseIncludesContentLength) {
    const std::string body = "{\"ok\":true}";
    const std::string response = RenderResponse(HttpStatus::Ok, body);
    EXPECT_TRUE(response.find("Content-Length: " + std::to_string(body.size())) !=
                std::string::npos);
}

TEST(RpcHttp, RenderResponseSendsConnectionClose) {
    const std::string response = RenderResponse(HttpStatus::Ok, "");
    EXPECT_TRUE(response.find("Connection: close") != std::string::npos);
}

TEST(RpcHttp, RenderResponseSendsNosniff) {
    const std::string response = RenderResponse(HttpStatus::Ok, "");
    EXPECT_TRUE(response.find("X-Content-Type-Options: nosniff") != std::string::npos);
}

TEST(RpcHttp, RenderResponseForUnauthorizedIncludesWwwAuthenticate) {
    const std::string response = RenderResponse(HttpStatus::Unauthorized, "");
    EXPECT_TRUE(response.find("WWW-Authenticate: Basic realm=\"amarian\"") !=
                std::string::npos);
}

TEST(RpcHttp, RenderResponseForOkDoesNotIncludeWwwAuthenticate) {
    const std::string response = RenderResponse(HttpStatus::Ok, "");
    EXPECT_TRUE(response.find("WWW-Authenticate") == std::string::npos);
}

TEST(RpcHttp, RenderResponseContainsBody) {
    const std::string body = "hello";
    const std::string response = RenderResponse(HttpStatus::Ok, body);
    EXPECT_TRUE(response.find(body) != std::string::npos);
}

TEST(RpcHttp, RenderResponseBodyFollowsCrlfCrlf) {
    const std::string body = "data";
    const std::string response = RenderResponse(HttpStatus::Ok, body);
    EXPECT_EQ(response.substr(response.find("\r\n\r\n") + 4), body);
}

TEST(RpcHttp, RenderRefusalIsPlainText) {
    const std::string response = RenderRefusal(HttpStatus::Unauthorized);
    EXPECT_TRUE(response.find("text/plain") != std::string::npos);
    EXPECT_TRUE(response.find("401") != std::string::npos);
}

TEST(RpcHttp, AllReasonPhrasesAreNonEmpty) {
    EXPECT_FALSE(ReasonPhrase(HttpStatus::Ok).empty());
    EXPECT_FALSE(ReasonPhrase(HttpStatus::BadRequest).empty());
    EXPECT_FALSE(ReasonPhrase(HttpStatus::Unauthorized).empty());
    EXPECT_FALSE(ReasonPhrase(HttpStatus::Forbidden).empty());
    EXPECT_FALSE(ReasonPhrase(HttpStatus::NotFound).empty());
    EXPECT_FALSE(ReasonPhrase(HttpStatus::MethodNotAllowed).empty());
    EXPECT_FALSE(ReasonPhrase(HttpStatus::PayloadTooLarge).empty());
    EXPECT_FALSE(ReasonPhrase(HttpStatus::UnsupportedMediaType).empty());
}

}  // namespace
}  // namespace amarian