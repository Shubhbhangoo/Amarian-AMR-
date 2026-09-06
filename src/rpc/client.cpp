/// \file
/// The client's byte-level half. See `rpc/client.hpp` for why it is a separate file from the
/// socket that carries it.

#include <amarian/rpc/client.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace amarian::rpc {
namespace {

using nlohmann::json;

constexpr std::string_view HEADER_TERMINATOR = "\r\n\r\n";
constexpr std::string_view LINE_TERMINATOR = "\r\n";

/// The longest run of digits this client will still read as a number.
///
/// Twenty is every value a `uint64_t` can hold, so nothing a method legitimately takes as a
/// number is excluded. Past it, digits are a byte string — see `ParseArgument`.
constexpr size_t MAX_NUMERIC_DIGITS = 20;

[[nodiscard]] char AsciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool IEquals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char x, char y) { return AsciiLower(x) == AsciiLower(y); });
}

[[nodiscard]] std::string_view TrimOws(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// `text` as a size, or nothing. Strict for the reason the server's copy is strict: a length
/// two parsers would disagree about is not a length worth honouring, and here the disagreement
/// would be between this client and whatever wrote the reply.
[[nodiscard]] std::optional<size_t> ParseLength(std::string_view text, size_t limit) noexcept {
    if (text.empty() || text.size() > MAX_NUMERIC_DIGITS) {
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

[[nodiscard]] ParsedResponse Fail(std::string failure) {
    ParsedResponse parsed;
    parsed.state = ResponseState::Malformed;
    parsed.failure = std::move(failure);
    return parsed;
}

/// What both entry points share: split the reply, and report the body along with whether the
/// announced length was reached.
///
/// `at_eof` decides the one question the bytes cannot answer. A reply that announced no length
/// is delimited by the close, so the same buffer is `NeedMore` while the connection is open
/// and `Complete` once it is not — which is why this is a parameter rather than a guess.
[[nodiscard]] ParsedResponse Split(std::string_view buffer, bool at_eof) {
    const size_t header_end = buffer.find(HEADER_TERMINATOR);
    if (header_end == std::string_view::npos) {
        if (buffer.size() > MAX_RESPONSE_BYTES) {
            return Fail("the reply's headers are larger than this client will read");
        }
        if (at_eof) {
            return Fail(buffer.empty() ? "the node closed the connection without replying"
                                       : "the node closed the connection mid-reply");
        }
        ParsedResponse parsed;
        parsed.state = ResponseState::NeedMore;
        return parsed;
    }

    const std::string_view head = buffer.substr(0, header_end);
    const size_t status_line_end = head.find(LINE_TERMINATOR);
    const std::string_view status_line =
        status_line_end == std::string_view::npos ? head : head.substr(0, status_line_end);

    // HTTP-VERSION SP STATUS SP REASON, and the reason phrase may be empty or absent.
    const size_t first_space = status_line.find(' ');
    if (first_space == std::string_view::npos || !status_line.starts_with("HTTP/1.")) {
        return Fail("the reply does not begin with an HTTP status line");
    }
    std::string_view code_text = status_line.substr(first_space + 1);
    if (const size_t second_space = code_text.find(' '); second_space != std::string_view::npos) {
        code_text = code_text.substr(0, second_space);
    }
    const std::optional<size_t> code = ParseLength(code_text, 999);
    if (!code.has_value() || *code < 100) {
        return Fail("the reply's status is not a status code");
    }

    // Only the one header this client acts on. The rest are ignored rather than collected: a
    // client that read `Connection` or `Content-Type` would be a client with a policy about
    // them, and it has none — it reads one reply and exits.
    std::optional<size_t> length;
    size_t length_count = 0;
    std::string_view rest = status_line_end == std::string_view::npos
                                ? std::string_view{}
                                : head.substr(status_line_end + LINE_TERMINATOR.size());
    while (!rest.empty()) {
        const size_t line_end = rest.find(LINE_TERMINATOR);
        const std::string_view line =
            line_end == std::string_view::npos ? rest : rest.substr(0, line_end);
        rest = line_end == std::string_view::npos
                   ? std::string_view{}
                   : rest.substr(line_end + LINE_TERMINATOR.size());
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        if (IEquals(line.substr(0, colon), "Content-Length")) {
            ++length_count;
            length = ParseLength(TrimOws(line.substr(colon + 1)), MAX_RESPONSE_BYTES);
        }
    }
    if (length_count > 1) {
        // Refused rather than resolved, exactly as the server refuses it. Two lengths is a
        // reply whose body two readers would delimit differently, and this client has no more
        // business guessing which is meant than the server has.
        return Fail("the reply announced its length twice");
    }
    if (length_count == 1 && !length.has_value()) {
        return Fail("the reply's length is not a number this client can read");
    }

    const size_t body_begin = header_end + HEADER_TERMINATOR.size();
    const size_t available = buffer.size() - body_begin;
    if (length.has_value() && available < *length) {
        if (at_eof) {
            return Fail("the node closed the connection before sending the whole reply");
        }
        ParsedResponse parsed;
        parsed.state = ResponseState::NeedMore;
        return parsed;
    }
    if (!length.has_value() && !at_eof) {
        // No length announced, so the body ends when the connection does and there is no way
        // to know from these bytes that it has.
        if (buffer.size() > MAX_RESPONSE_BYTES) {
            return Fail("the reply is larger than this client will read");
        }
        ParsedResponse parsed;
        parsed.state = ResponseState::NeedMore;
        return parsed;
    }

    ParsedResponse parsed;
    parsed.state = ResponseState::Complete;
    parsed.status = static_cast<int>(*code);
    parsed.body = std::string(buffer.substr(body_begin, length.value_or(available)));
    return parsed;
}

}  // namespace

std::string RenderCallBody(std::string_view method, const json& params, int64_t id) {
    json request = json::object();
    // Announced as 2.0 because that is what this is, even though the server does not require
    // the field: a client that omitted it would still work here and would break against the
    // first proxy or library that checks.
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = std::string(method);
    request["params"] = params;
    return request.dump();
}

std::string RenderCall(std::string_view authorization, std::string_view host, uint16_t port,
                       std::string_view body) {
    std::string request;
    request.reserve(body.size() + 256);
    request += "POST / HTTP/1.1\r\n";
    request += "Host: ";
    request += host;
    request += ':';
    request += std::to_string(port);
    request += "\r\n";
    request += "Authorization: ";
    request += authorization;
    request += "\r\n";
    request += "Content-Type: application/json\r\n";
    // Stated rather than left to the server's own policy. The server closes after one reply
    // whatever a client asks for, and a client that did not say so would be describing a
    // conversation it is not having.
    request += "Connection: close\r\n";
    request += "Content-Length: ";
    request += std::to_string(body.size());
    request += HEADER_TERMINATOR;
    request += body;
    return request;
}

ParsedResponse ParseResponse(std::string_view buffer) { return Split(buffer, false); }

ParsedResponse FinishResponse(std::string_view buffer) { return Split(buffer, true); }

json ParseArgument(std::string_view text) {
    // A long digit run is a byte string. See the header: this is the one place the
    // parse-as-JSON rule is wrong often enough to be worth a guard, because a block in hex is
    // all digits about once in every sixteen million.
    const bool all_digits = !text.empty() && std::ranges::all_of(text, [](char c) {
        return c >= '0' && c <= '9';
    });
    if (all_digits && text.size() > MAX_NUMERIC_DIGITS) {
        return json(std::string(text));
    }

    // Parsed without exceptions, because a word that is not JSON is the common case here and
    // not an error: `getblockhash 0` and `submitblock deadbeef` both arrive through this
    // function, and only one of them is JSON.
    json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        return json(std::string(text));
    }
    return parsed;
}

}  // namespace amarian::rpc
