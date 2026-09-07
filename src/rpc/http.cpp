#include <amarian/rpc/http.hpp>

#include <amarian/crypto/random.hpp>
#include <amarian/util/hex.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace amarian::rpc {
namespace {

using nlohmann::json;

constexpr std::string_view HEADER_TERMINATOR = "\r\n\r\n";
constexpr std::string_view LINE_TERMINATOR = "\r\n";

/// ASCII lowercase, and only ASCII. `std::tolower` consults the locale, which means the same
/// header name could compare differently on two machines with the same build — a property no
/// part of a protocol may have.
[[nodiscard]] char AsciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (AsciiLower(a[i]) != AsciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

/// Strips the optional whitespace HTTP permits around a header value. Spaces and horizontal
/// tabs only: a stray carriage return inside a value is a framing error, not whitespace, and
/// is refused where the line is split rather than quietly trimmed here.
[[nodiscard]] std::string_view TrimOws(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// A header name is a token: no spaces, no controls. Refused rather than trimmed, because
/// `Content-Length : 5` accepted by one parser and rejected by another is precisely how a
/// request smuggles a second body past a proxy.
[[nodiscard]] bool IsTokenName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        if (c <= ' ' || c == 0x7F || c == ':') {
            return false;
        }
    }
    return true;
}

/// `text` as a size, or nothing if it is not exactly a bounded run of decimal digits.
///
/// Deliberately stricter than `strtoull`: no sign, no whitespace, no trailing text, no
/// hexadecimal. A `Content-Length` of `+5` or `0x10` is a request two parsers will disagree
/// about, and disagreement is the vulnerability.
[[nodiscard]] std::optional<size_t> ParseLength(std::string_view text, size_t limit) noexcept {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    size_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const size_t digit = static_cast<size_t>(c - '0');
        if (value > (limit - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

/// How many times a header appears. Any of the headers this server acts on appearing twice is
/// refused: one parser reading the first and another the second is the whole of request
/// smuggling, and no legitimate client sends two.
[[nodiscard]] size_t CountHeader(const HttpRequest& request, std::string_view name) noexcept {
    size_t count = 0;
    for (const auto& [key, value] : request.headers) {
        static_cast<void>(value);
        if (IEquals(key, name)) {
            ++count;
        }
    }
    return count;
}

/// Whether `host` names this machine, at this port.
///
/// Only three literal names are accepted, and a name is not resolved: resolution is the
/// attacker's tool here, so a check that resolved would be checking the wrong thing. The port
/// must match what this server is bound to, because a client that knows where it connected
/// sends that port and a browser directed at a rebound name does not have to.
[[nodiscard]] bool IsLoopbackHost(std::string_view host, uint16_t port) noexcept {
    const size_t colon = host.rfind(':');
    const size_t bracket = host.rfind(']');
    // A trailing `:port` is present when the last colon is outside any bracketed address and
    // is the only colon — an unbracketed address with two of them is not a name accepted here
    // anyway, and treating its last colon as a separator would silently accept `::1`.
    const bool has_port = colon != std::string_view::npos &&
                          (bracket == std::string_view::npos ? host.find(':') == colon
                                                             : colon > bracket);
    std::string_view name = host;
    if (has_port) {
        const std::optional<size_t> given = ParseLength(host.substr(colon + 1), 65535);
        if (!given.has_value() || static_cast<uint16_t>(*given) != port) {
            return false;
        }
        name = host.substr(0, colon);
    }
    return name == "127.0.0.1" || name == "localhost" || name == "[::1]";
}

/// A JSON-RPC error object.
[[nodiscard]] json MakeError(int code, std::string_view message) {
    json error = json::object();
    error["code"] = code;
    error["message"] = std::string(message);
    return error;
}

/// The reply envelope: Bitcoin Core's shape, which is what every miner and JSON-RPC client
/// already speaks. Exactly one of `result` and `error` is non-null, always — a reply with
/// both or neither is one a client has to guess about.
[[nodiscard]] json Envelope(const json& id, json payload, bool is_result = false) {
    json reply = json::object();
    reply["result"] = is_result ? std::move(payload) : json();
    reply["error"] = is_result ? json() : std::move(payload);
    reply["id"] = id;
    return reply;
}

[[nodiscard]] Framed Refuse(Framing state, HttpStatus status) {
    Framed framed;
    framed.state = state;
    framed.status = status;
    return framed;
}

}  // namespace

std::string_view ReasonPhrase(HttpStatus status) noexcept {
    switch (status) {
        case HttpStatus::Ok:
            return "OK";
        case HttpStatus::BadRequest:
            return "Bad Request";
        case HttpStatus::Unauthorized:
            return "Unauthorized";
        case HttpStatus::Forbidden:
            return "Forbidden";
        case HttpStatus::NotFound:
            return "Not Found";
        case HttpStatus::MethodNotAllowed:
            return "Method Not Allowed";
        case HttpStatus::PayloadTooLarge:
            return "Payload Too Large";
        case HttpStatus::UnsupportedMediaType:
            return "Unsupported Media Type";
    }
    return "Error";
}

const std::string* HttpRequest::Header(std::string_view name) const noexcept {
    for (const auto& [key, value] : headers) {
        if (IEquals(key, name)) {
            return &value;
        }
    }
    return nullptr;
}

Framed FrameRequest(std::string_view buffer) {
    const size_t header_end = buffer.find(HEADER_TERMINATOR);
    if (header_end == std::string_view::npos) {
        // Not yet a whole header section. Bounded so that a client which never sends the
        // blank line cannot make this server hold an unbounded buffer for it.
        if (buffer.size() > MAX_HEADER_BYTES) {
            return Refuse(Framing::TooLarge, HttpStatus::PayloadTooLarge);
        }
        return Refuse(Framing::NeedMore, HttpStatus::Ok);
    }
    if (header_end > MAX_HEADER_BYTES) {
        return Refuse(Framing::TooLarge, HttpStatus::PayloadTooLarge);
    }

    const std::string_view head = buffer.substr(0, header_end);
    const size_t request_line_end = head.find(LINE_TERMINATOR);
    const std::string_view request_line =
        request_line_end == std::string_view::npos ? head : head.substr(0, request_line_end);

    // METHOD SP TARGET SP VERSION, with exactly one space either side and nothing optional.
    const size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) {
        return Refuse(Framing::Malformed, HttpStatus::BadRequest);
    }
    const size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return Refuse(Framing::Malformed, HttpStatus::BadRequest);
    }
    const std::string_view version = request_line.substr(second_space + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        return Refuse(Framing::Malformed, HttpStatus::BadRequest);
    }

    Framed framed;
    framed.request.method = std::string(request_line.substr(0, first_space));
    framed.request.target =
        std::string(request_line.substr(first_space + 1, second_space - first_space - 1));
    if (framed.request.target.empty() || framed.request.target.front() != '/') {
        // An absolute-form target is what a request to a proxy looks like. This is not one.
        return Refuse(Framing::Malformed, HttpStatus::BadRequest);
    }

    std::string_view rest =
        request_line_end == std::string_view::npos
            ? std::string_view{}
            : head.substr(request_line_end + LINE_TERMINATOR.size());
    while (!rest.empty()) {
        const size_t line_end = rest.find(LINE_TERMINATOR);
        const std::string_view line =
            line_end == std::string_view::npos ? rest : rest.substr(0, line_end);
        rest = line_end == std::string_view::npos
                   ? std::string_view{}
                   : rest.substr(line_end + LINE_TERMINATOR.size());
        if (line.empty()) {
            continue;
        }
        if (line.front() == ' ' || line.front() == '\t') {
            // An obsolete folded header. Deleted from HTTP in RFC 7230 and refused here,
            // because a value spanning lines is a value two parsers will split differently.
            return Refuse(Framing::Malformed, HttpStatus::BadRequest);
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return Refuse(Framing::Malformed, HttpStatus::BadRequest);
        }
        const std::string_view name = line.substr(0, colon);
        if (!IsTokenName(name)) {
            return Refuse(Framing::Malformed, HttpStatus::BadRequest);
        }
        framed.request.headers.emplace_back(std::string(name),
                                            std::string(TrimOws(line.substr(colon + 1))));
    }

    // Nothing that changes how the body is delimited except `Content-Length`. Chunked
    // encoding is a second framing to get right and nothing a local client needs.
    if (CountHeader(framed.request, "Transfer-Encoding") != 0) {
        return Refuse(Framing::Malformed, HttpStatus::BadRequest);
    }
    if (CountHeader(framed.request, "Content-Length") != 1) {
        return Refuse(Framing::Malformed, HttpStatus::BadRequest);
    }
    const std::string* length_text = framed.request.Header("Content-Length");
    const std::optional<size_t> length = ParseLength(*length_text, MAX_REQUEST_BYTES);
    if (!length.has_value()) {
        // Either not a number or larger than this server will hold. Both refuse the request;
        // the size case says so specifically, because an operator submitting an oversized
        // block deserves to be told which of the two happened.
        const bool numeric = !length_text->empty() &&
                             std::all_of(length_text->begin(), length_text->end(),
                                         [](char c) { return c >= '0' && c <= '9'; });
        return Refuse(numeric ? Framing::TooLarge : Framing::Malformed,
                      numeric ? HttpStatus::PayloadTooLarge : HttpStatus::BadRequest);
    }

    const size_t body_begin = header_end + HEADER_TERMINATOR.size();
    if (buffer.size() < body_begin + *length) {
        return Refuse(Framing::NeedMore, HttpStatus::Ok);
    }
    framed.request.body = std::string(buffer.substr(body_begin, *length));
    framed.state = Framing::Complete;
    framed.consumed = body_begin + *length;
    return framed;
}

std::optional<HttpStatus> Screen(const HttpRequest& request, const AuthPolicy& policy) {
    // Authentication first, so that a caller who cannot authenticate learns nothing about
    // what this server would otherwise have said about its request.
    if (CountHeader(request, "Authorization") != 1) {
        return HttpStatus::Unauthorized;
    }
    if (!ConstantTimeEquals(*request.Header("Authorization"), policy.expected_authorization)) {
        return HttpStatus::Unauthorized;
    }

    // The `Host` check, against DNS rebinding. A name the attacker controls that resolves to
    // 127.0.0.1 makes the operator's browser into a proxy for this interface, and the
    // connection really does arrive from loopback — the only thing that distinguishes it is
    // the name the browser was told to ask for.
    if (CountHeader(request, "Host") != 1) {
        return HttpStatus::Forbidden;
    }
    if (!IsLoopbackHost(*request.Header("Host"), policy.port)) {
        return HttpStatus::Forbidden;
    }

    if (request.method != "POST") {
        // A GET is what a browser, a link preview or a crawler issues, so refusing it means a
        // URL on its own can never be an RPC call.
        return HttpStatus::MethodNotAllowed;
    }
    if (request.target != "/") {
        return HttpStatus::NotFound;
    }

    // A cross-origin form post is the one request a browser will make without asking
    // permission first, and its content type is never this one.
    const std::string* content_type = request.Header("Content-Type");
    if (content_type == nullptr) {
        return HttpStatus::UnsupportedMediaType;
    }
    const std::string_view media =
        TrimOws(std::string_view(*content_type).substr(0, content_type->find(';')));
    if (!IEquals(media, "application/json")) {
        return HttpStatus::UnsupportedMediaType;
    }
    return std::nullopt;
}

std::string RenderResponse(HttpStatus status, std::string_view body,
                           std::string_view content_type) {
    std::string out = "HTTP/1.1 ";
    out += std::to_string(static_cast<int>(status));
    out += ' ';
    out += ReasonPhrase(status);
    out += "\r\nContent-Type: ";
    out += content_type;
    out += "\r\nContent-Length: ";
    out += std::to_string(body.size());
    // Stated in every response because this server closes after one request whether or not the
    // client asked it to, and a client that assumed otherwise would hang waiting for a second
    // answer on a socket that is gone.
    out += "\r\nConnection: close";
    // The interface answers JSON to a program. Telling a browser not to guess otherwise costs
    // one header and removes a class of confusion attacks entirely.
    out += "\r\nX-Content-Type-Options: nosniff";
    if (status == HttpStatus::Unauthorized) {
        // Without this a client cannot tell an authentication failure from a refusal it should
        // not retry, and every HTTP library's credential handling is built around seeing it.
        out += "\r\nWWW-Authenticate: Basic realm=\"amarian\"";
    }
    out += "\r\n\r\n";
    out += body;
    return out;
}

std::string RenderRefusal(HttpStatus status) {
    // The status and its reason phrase, which is everything a refused client is entitled to
    // know: it either has a credential and sent a bad request, in which case the status says
    // which of this server's four requirements it missed, or it has no credential, in which
    // case any further detail would be a description of an interface it may not use.
    std::string body = std::to_string(static_cast<int>(status));
    body += ' ';
    body += ReasonPhrase(status);
    body += '\n';
    return RenderResponse(status, body, "text/plain; charset=utf-8");
}

RpcAnswer AnswerJsonRpc(Node& node, std::string_view body, int64_t now) {
    // Parsed without exceptions: a malformed body is the most ordinary thing a client can send
    // and must not travel through the same mechanism as a bug.
    const json request = json::parse(body, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
        return RpcAnswer{.status = HttpStatus::BadRequest,
                         .body = Envelope(json(), MakeError(-32700, "not a JSON object")).dump()};
    }

    // The id travels back exactly as it arrived, whatever it was, including absent: a client
    // matching replies to requests is entitled to its own convention, and normalising the
    // field would break the match.
    const json id = request.contains("id") ? request.at("id") : json();

    const auto method = request.find("method");
    if (method == request.end() || !method->is_string()) {
        return RpcAnswer{.status = HttpStatus::BadRequest,
                         .body = Envelope(id, MakeError(-32600, "no method named")).dump()};
    }

    // Absent params is the empty argument list, which is what a method taking none expects to
    // be handed. Anything that is neither absent, null, an array nor an object is a request
    // this server would have to guess at.
    json params = json::array();
    if (const auto given = request.find("params"); given != request.end() && !given->is_null()) {
        if (!given->is_array() && !given->is_object()) {
            return RpcAnswer{
                .status = HttpStatus::BadRequest,
                .body = Envelope(id, MakeError(-32602, "params must be an array or an object"))
                            .dump()};
        }
        params = *given;
    }

    const Result result = Dispatch(node, method->get<std::string>(), params, now);
    if (result.has_value()) {
        return RpcAnswer{.status = HttpStatus::Ok, .body = Envelope(id, *result, true).dump()};
    }

    json error = MakeError(Code(result.error().code), Describe(result.error().code));
    if (!result.error().detail.empty()) {
        error["data"] = std::string(result.error().detail);
    }
    // 404 for a method that does not exist, because that is what the status means and what
    // every JSON-RPC client checks for. Everything else is a well-formed request this node
    // answered with a refusal, and a refusal is a result: 200 with the error object, so that a
    // client which treats a non-2xx status as a transport failure still reads the reason.
    const HttpStatus status = result.error().code == RpcError::UnknownMethod
                                  ? HttpStatus::NotFound
                                  : HttpStatus::Ok;
    return RpcAnswer{.status = status, .body = Envelope(id, std::move(error)).dump()};
}

bool ConstantTimeEquals(std::string_view a, std::string_view b) noexcept {
    // The length difference is folded in rather than short-circuited on, but a difference in
    // length is still observable in how long the loop runs. That is deliberate and harmless
    // here: the credential's length is a fixed property of the format, not a secret. What must
    // not leak is *where* two equal-length strings first differ, because that is a measurement
    // of how much of the token the caller guessed correctly.
    unsigned int diff = a.size() == b.size() ? 0U : 1U;
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i) {
        diff |= static_cast<unsigned int>(static_cast<unsigned char>(a[i]) ^
                                          static_cast<unsigned char>(b[i]));
    }
    return diff == 0;
}

std::string Base64Encode(std::string_view bytes) {
    static constexpr std::string_view ALPHABET =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < bytes.size()) {
        const uint32_t group = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                               (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                               static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
        out += ALPHABET[(group >> 18) & 0x3FU];
        out += ALPHABET[(group >> 12) & 0x3FU];
        out += ALPHABET[(group >> 6) & 0x3FU];
        out += ALPHABET[group & 0x3FU];
        i += 3;
    }
    const size_t remaining = bytes.size() - i;
    if (remaining != 0) {
        uint32_t group = static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
        if (remaining == 2) {
            group |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8;
        }
        out += ALPHABET[(group >> 18) & 0x3FU];
        out += ALPHABET[(group >> 12) & 0x3FU];
        out += remaining == 2 ? ALPHABET[(group >> 6) & 0x3FU] : '=';
        out += '=';
    }
    return out;
}

namespace {

/// Writes `contents` to `path` as a file only the account that created it can read.
///
/// The ordering is the whole point. Permissions are narrowed while the file is still empty, so
/// the token is never on disk under a mode another account could read — a file created and then
/// chmodded after the write has already been readable, and "briefly" is not a security
/// property. The already-open stream is unaffected by the change, which is why the write can
/// follow it.
[[nodiscard]] std::optional<std::string> WriteOwnerOnly(const std::filesystem::path& path,
                                                        std::string_view contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return "cannot create " + path.string();
    }
    std::error_code ec;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        return "cannot restrict the permissions on " + path.string() + ": " + ec.message();
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    if (!out) {
        return "cannot write " + path.string();
    }
    return std::nullopt;
}

}  // namespace

std::expected<Cookie, std::string> Cookie::Generate(const std::filesystem::path& path) {
    // 32 bytes, which is 256 bits of an attacker's search space and the same width Bitcoin
    // Core writes. There is no argument for less and no benefit to more.
    constexpr size_t TOKEN_BYTES = 32;
    constexpr std::string_view COOKIE_USER = "__cookie__";

    const ByteVec token = crypto::RandomByteVec(TOKEN_BYTES);
    if (token.size() != TOKEN_BYTES) {
        return std::unexpected(
            "the platform's entropy source failed, so no RPC cookie could be generated");
    }

    Cookie cookie;
    // The user half is a constant. HTTP Basic wants a `user:password` pair and this is the
    // whole of it, so there is nothing for a caller to configure and nothing for an operator
    // to get wrong; the token is the only part that carries any authority.
    cookie.credential_ = std::string(COOKIE_USER) + ":" + ToHex(token);

    // Written under a temporary name and renamed into place, because a rename is atomic on the
    // filesystems this node runs on: a client either finds the previous cookie or this one, and
    // never a file holding half a token that it would then fail to authenticate with.
    std::filesystem::path temp = path;
    temp += ".tmp";

    std::error_code ec;
    std::filesystem::remove(temp, ec);
    if (const std::optional<std::string> failure = WriteOwnerOnly(temp, cookie.credential_);
        failure.has_value()) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(*failure);
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return std::unexpected("cannot move the cookie into place at " + path.string() + ": " +
                               ec.message());
    }
    return cookie;
}

std::expected<Cookie, std::string> Cookie::Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::unexpected("cannot open " + path.string() +
                               " (is the node running, and is this its datadir?)");
    }
    // Bounded, and a file that fills the buffer is refused rather than truncated. A credential
    // read from the first 256 bytes of something larger would be a wrong credential presented
    // as a right one, and the resulting 401 would send the operator looking at the wrong thing.
    std::array<char, 256> buffer{};
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (in.bad()) {
        return std::unexpected("cannot read " + path.string());
    }
    const size_t got = static_cast<size_t>(in.gcount());
    if (got == buffer.size()) {
        return std::unexpected(path.string() + " is too large to be a cookie file");
    }

    std::string_view contents(buffer.data(), got);
    // A trailing end-of-line is trimmed because an editor may have added one. Nothing else is:
    // a credential's bytes are its bytes, and a server trimming more than it must is a server
    // accepting tokens it was not given.
    while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r')) {
        contents.remove_suffix(1);
    }
    if (contents.empty()) {
        return std::unexpected(path.string() + " is empty");
    }
    if (contents.find(':') == std::string_view::npos) {
        return std::unexpected(path.string() + " does not hold a `user:token` credential");
    }

    Cookie cookie;
    cookie.credential_ = std::string(contents);
    return cookie;
}

std::string Cookie::Authorization() const {
    return "Basic " + Base64Encode(credential_);
}

}  // namespace amarian::rpc