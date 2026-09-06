#pragma once

/// \file
/// The client side of the same transport, and like the server side, no socket.
///
/// This is the half of `amarian-cli` that is a function of bytes: what a call looks like on
/// the wire, how a reply is framed, and how a word typed on a command line becomes a JSON
/// argument. It is separate from the socket for the reason the server's half is —
/// `rpc/http.hpp` explains it at length — but the emphasis differs. There, the decisions kept
/// away from the socket were about what to refuse. Here they are about what a request *means*,
/// and the one that matters is `ParseArgument`: a mistake in it sends the node a different
/// call than the operator typed, and the node has no way to notice.
///
/// A client also gets to be stricter than a server. This one talks to exactly one
/// implementation, which it can assume announces a length and closes afterwards, so there is
/// no chunked decoding and no keep-alive here either — not as a limitation but because the
/// server on the other end has none to offer.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace amarian::rpc {

/// The largest reply this client will hold.
///
/// Generous next to `MAX_REQUEST_BYTES`, and deliberately so: the largest legitimate reply is
/// a `getblocktemplate` carrying a full block's transactions as hex, which is twice the bytes
/// of the largest request. The bound exists because a client that trusts a length it was given
/// is a client any process that wins the race to the port can make consume all of memory.
constexpr size_t MAX_RESPONSE_BYTES = 16U << 20;

/// The JSON-RPC request body for one call.
///
/// `id` is echoed by the server untouched, so this client sends one and checks it came back:
/// with a single request per connection a mismatch cannot mean a crossed reply, but it can
/// mean the answer came from something that is not this node's interface, and that is worth
/// knowing before the output is printed as though it were.
[[nodiscard]] std::string RenderCallBody(std::string_view method, const nlohmann::json& params,
                                         int64_t id);

/// The whole HTTP request, headers and body, for one call.
///
/// `Host` is rendered from `host` and `port` rather than from the address actually connected
/// to, because the server checks it against the port it is bound to and a client that lied
/// here would be refused. That refusal is the check working: the two must agree.
[[nodiscard]] std::string RenderCall(std::string_view authorization, std::string_view host,
                                     uint16_t port, std::string_view body);

/// How far `ParseResponse` got.
enum class ResponseState {
    /// The buffer holds a whole reply. `status` and `body` are it.
    Complete,
    /// A prefix of one. The caller should read more bytes.
    NeedMore,
    /// Not a reply this client can read, or one too large to hold.
    Malformed,
};

struct ParsedResponse {
    ResponseState state = ResponseState::NeedMore;
    /// The HTTP status, once `Complete`.
    int status = 0;
    std::string body;
    /// Why, when `Malformed`. For the operator, so a client that cannot read a reply says
    /// something more useful than that it could not.
    std::string failure;
};

/// Frames one reply out of whatever has arrived so far.
///
/// Restartable, exactly as `FrameRequest` is, and for the same reason: the caller reads into a
/// growing buffer and asks again, so there is no parser state to get wrong between reads.
///
/// A reply with no `Content-Length` is read until the connection closes, which is legitimate
/// HTTP/1.1 and is why `NeedMore` cannot be distinguished from a finished bodyless reply
/// without knowing the socket closed. The caller resolves that by calling `FinishResponse`
/// when its read ends in end-of-file.
[[nodiscard]] ParsedResponse ParseResponse(std::string_view buffer);

/// The same, for a buffer the peer has finished sending.
///
/// Called when a read returns end-of-file: a reply that was `NeedMore` only because no length
/// was announced is complete, and one that was short of an announced length is truncated and
/// says so. Splitting this out is what keeps `ParseResponse` a pure function of the bytes
/// rather than one that also needs to be told about the socket.
[[nodiscard]] ParsedResponse FinishResponse(std::string_view buffer);

/// One command-line word as a JSON argument.
///
/// The rule is the one Bitcoin Core's client uses, because it is what an operator's fingers
/// already expect: a word that parses as JSON is that JSON, and a word that does not is a
/// string. So `2` is the number two, `[]` is an empty array, `true` is a boolean, and
/// `deadbeef` is the string "deadbeef".
///
/// With one guard Core does not have, against the one case where the rule is actively wrong.
/// A long run of decimal digits is a byte string that happens to contain no letters — a block
/// or a transaction in hex — and never a number, because no argument any method here takes is
/// a number of more than twenty digits. Without the guard, one block in sixteen million would
/// be submitted as an integer and refused for the wrong reason. A hex string can still be
/// forced with quotes at any length; the guard only decides the default.
[[nodiscard]] nlohmann::json ParseArgument(std::string_view text);

}  // namespace amarian::rpc
