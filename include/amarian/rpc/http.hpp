#pragma once

/// \file
/// The HTTP framing, authentication and JSON-RPC envelope of the node's RPC transport.
///
/// Everything here is a function of bytes, and that is the point of the file existing apart
/// from the listener: the decisions that keep this interface from being a way into the
/// machine — which requests are refused, and on what evidence — are decisions about a request
/// rather than about a socket, so they are made and tested here, and `rpc::server` is left
/// with nothing but reading, writing and closing.
///
/// ## What is refused, and why
///
/// The RPC interface can create blocks and, when a wallet exists, move money. It listens on
/// loopback only, but loopback is not a boundary: any process on the machine can reach it, and
/// so can any web page in the operator's browser. So a request is refused unless every one of
/// these holds, and each guards against something specific:
///
///   * Exactly one `Content-Length`, and no `Transfer-Encoding`. This is a framing requirement
///     rather than a policy one, so it is answered first and with `400`: a request whose body
///     two parsers would delimit differently is the request that smuggles a second one past a
///     proxy. It is also why a `GET` never reaches the method check below — a bodiless request
///     has no `Content-Length` and is refused here, one step earlier than one might expect.
///   * `POST` to `/`. A `GET` is what a browser or a link-preview fetcher issues, and refusing
///     it means a URL alone can never be an RPC call. The check is reached by a `GET` that
///     carries a length, and it is what refuses every other method outright.
///   * `Content-Type: application/json`. A form post is the one cross-origin request a browser
///     will make without asking permission first, and its content type is never this one.
///   * A `Host` header naming loopback. Without this check, a DNS name the attacker controls
///     that resolves to 127.0.0.1 turns the operator's browser into a proxy for this
///     interface — the request arrives from the loopback address because the browser really
///     is on this machine.
///   * `Authorization: Basic` carrying the cookie token, compared in constant time. The cookie
///     is a file only the operator can read, which is what makes "on this machine" insufficient
///     and "running as this user" the actual requirement.
///
/// Of the last four, authentication is checked first, so that a caller who cannot authenticate
/// learns nothing about what this server would have said about its request.

///
/// Nothing is refused on the basis of a header this file does not name: an unrecognised header
/// is ignored, because a client that adds one is not thereby suspicious and a server that
/// refuses one is a server nobody can talk to.
///
/// ## What it deliberately does not do
///
/// No keep-alive, no pipelining, no chunked encoding, no compression, no batches. Each is a
/// state machine whose bugs are somebody's exploit, and none buys a local node anything: the
/// caller is a miner asking for one template or a CLI asking one question. One request per
/// connection, `Content-Length` required, connection closed after the answer.

#include <amarian/rpc/dispatch.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amarian::rpc {

/// The largest request this server will hold in memory.
///
/// A submitted block is the largest legitimate request, and at `MAX_BLOCK_WEIGHT` it is at
/// most two million bytes, which is four million hex digits. Eight mebibytes leaves room for
/// the JSON around it and refuses anything that cannot be a block.
constexpr size_t MAX_REQUEST_BYTES = 8U << 20;

/// The largest header section. A client with more than this to say about a request that is
/// itself capped is not a client.
constexpr size_t MAX_HEADER_BYTES = 8192;

/// The statuses this server can answer with. `-Wswitch-enum` makes every switch over this
/// total, which is why there are no unused members.
enum class HttpStatus : int {
    Ok = 200,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    PayloadTooLarge = 413,
    UnsupportedMediaType = 415,
};

/// The reason phrase, for the status line.
[[nodiscard]] std::string_view ReasonPhrase(HttpStatus status) noexcept;

/// A request, framed and split but not yet judged.
struct HttpRequest {
    std::string method;
    std::string target;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// The named header's value, or null. Case-insensitive, as HTTP requires. Returns the
    /// first occurrence: a repeated header is not merged, because none of the headers this
    /// server reads may legitimately appear twice and merging them is how a request smuggles
    /// a second value past a check that read the first.
    [[nodiscard]] const std::string* Header(std::string_view name) const noexcept;
};

/// How far `FrameRequest` got.
enum class Framing {
    /// The buffer holds a whole request. `Framed::request` is it.
    Complete,
    /// A prefix of a possibly-valid request. The caller should read more bytes.
    NeedMore,
    /// Not a request this server can parse. The caller should answer and close.
    Malformed,
    /// Within the syntax, but past a size bound.
    TooLarge,
};

struct Framed {
    Framing state = Framing::NeedMore;
    HttpRequest request;
    /// The status to answer with when `state` is `Malformed` or `TooLarge`.
    HttpStatus status = HttpStatus::BadRequest;
    /// How many bytes of `buffer` the framed request occupied. Only meaningful when
    /// `Complete`; anything after it is a second request this server will not read.
    size_t consumed = 0;
};

/// Frames one request out of whatever has arrived so far.
///
/// Pure and restartable: the listener calls it again after every read rather than keeping a
/// parser's worth of state between them, which is why a partial request cannot leave this
/// server in a state a second partial request can exploit.
[[nodiscard]] Framed FrameRequest(std::string_view buffer);

/// What a request must carry to be answered at all.
struct AuthPolicy {
    /// The expected `Authorization` header value in full, including the `Basic ` prefix.
    std::string expected_authorization;
    /// The port this server is bound to, so a `Host` header naming a different one is
    /// refused — it did not come from a client that knows where it is connected.
    uint16_t port = 0;
};

/// The status a request should be refused with, or nothing if it may be answered.
///
/// Separate from framing so that the two questions stay separate: framing asks whether these
/// bytes are a request, and this asks whether this request is allowed. A caller that skipped
/// this would still have a well-formed request and no error to report, which is the shape of
/// mistake that turns a check into a comment.
[[nodiscard]] std::optional<HttpStatus> Screen(const HttpRequest& request,
                                               const AuthPolicy& policy);

/// A rendered HTTP response: status line, the headers this server always sends, and the body.
///
/// `Connection: close` is not negotiable and is stated in every response, because this server
/// closes after one request whether or not the client asked it to.
[[nodiscard]] std::string RenderResponse(HttpStatus status, std::string_view body,
                                         std::string_view content_type = "application/json");

/// The whole response for a request refused before it reached a method.
///
/// Plain text, not JSON-RPC's error shape, and that is a deliberate claim about what happened:
/// a request refused here may not have been a JSON-RPC request at all — it may have been a
/// browser's `GET`, a form post, or bytes that are not a request in any protocol — and an
/// answer in JSON-RPC's envelope would assert a conversation that never took place. A client
/// that got this far without authenticating is told the status and nothing else.
[[nodiscard]] std::string RenderRefusal(HttpStatus status);

/// The answer to one JSON-RPC request body, and the HTTP status to send it with.
struct RpcAnswer {
    HttpStatus status = HttpStatus::Ok;
    std::string body;
};

/// Parses `body` as a JSON-RPC request, dispatches it, and renders the reply.
///
/// The reply is Bitcoin Core's shape — `{"result":…,"error":…,"id":…}`, with exactly one of
/// the first two non-null — because that is what every miner and every JSON-RPC client
/// already speaks, and inventing a second shape would buy nothing.
///
/// The `id` comes back exactly as it was sent, whatever it was, including null: a client
/// matching replies to requests is entitled to its own convention, and a server that
/// normalised the field would break the match.
[[nodiscard]] RpcAnswer AnswerJsonRpc(Node& node, std::string_view body, int64_t now);

/// The shared secret a client must present, and the file it lives in.
///
/// A cookie rather than a configured password for the reason Bitcoin Core adopted one: a
/// password in a config file is a password that gets reused, committed, and left behind, and
/// one generated per run and readable only by the account that started the node cannot be any
/// of those. The token is 32 random bytes as hex, which is 128 bits of what an attacker on
/// this machine would have to guess before the node next restarts.
class Cookie {
public:
    /// Generates a token and writes `<datadir>/.cookie` with owner-only permissions.
    ///
    /// The error string is for the operator: every way this fails is something about their
    /// filesystem, and none of it is worth an enum.
    [[nodiscard]] static std::expected<Cookie, std::string> Generate(
        const std::filesystem::path& path);

    /// Reads a cookie a node wrote. For a client.
    [[nodiscard]] static std::expected<Cookie, std::string> Read(
        const std::filesystem::path& path);

    /// The `user:token` credential, as it appears before base64.
    [[nodiscard]] std::string_view Credential() const noexcept { return credential_; }

    /// The full `Authorization` header value a client must send.
    [[nodiscard]] std::string Authorization() const;

private:
    std::string credential_;
};

/// Base64, for the one place HTTP requires it. Not a general utility: the alphabet is
/// standard, padding is always emitted, and no line breaks are inserted.
[[nodiscard]] std::string Base64Encode(std::string_view bytes);

/// Whether two strings are equal, in time that does not depend on where they first differ.
///
/// The comparison a credential check needs. `std::string::operator==` returns as soon as it
/// finds a difference, and the time it takes to do that is a measurement of how much of the
/// token the caller got right — over enough attempts, that is the token.
[[nodiscard]] bool ConstantTimeEquals(std::string_view a, std::string_view b) noexcept;

}  // namespace amarian::rpc
