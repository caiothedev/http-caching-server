#include "http/parser.hpp"

#include <algorithm>

#include "util/strings.hpp"

namespace proxy::http {

    namespace {

        constexpr std::size_t kMaxChunkLine = 4096;

        bool parse_version(std::string_view s, Version& out) {
            // HTTP-version = "HTTP/" DIGIT "." DIGIT
            if (s.size() != 8 || s.substr(0, 5) != "HTTP/" || s[6] != '.') return false;
            if (s[5] < '0' || s[5] > '9' || s[7] < '0' || s[7] > '9') return false;
            out.major = s[5] - '0';
            out.minor = s[7] - '0';
            return true;
        }

        bool valid_field_value(std::string_view value) {
            for (unsigned char c : value) {
                if (c == '\t' || (c >= 0x20 && c != 0x7f)) continue;
                return false;
            }
            return true;
        }

        enum class TransferEncodingStatus { Supported, Unsupported, Invalid };

        TransferEncodingStatus parse_transfer_encoding(std::string_view value) {
            std::size_t count = 0;
            std::size_t start = 0;
            bool unsupported = false;
            while (start <= value.size()) {
                auto end = value.find(',', start);
                if (end == std::string_view::npos) end = value.size();
                auto coding = util::trim(value.substr(start, end - start));
                if (!util::is_token(coding)) return TransferEncodingStatus::Invalid;
                ++count;
                if (!util::iequals(coding, "chunked")) unsupported = true;
                if (end == value.size()) break;
                start = end + 1;
            }
            if (count == 0 || (count > 1 && !unsupported)) return TransferEncodingStatus::Invalid;
            return unsupported ? TransferEncodingStatus::Unsupported : TransferEncodingStatus::Supported;
        }

        bool parse_hex(std::string_view s, std::uint64_t& out) {
            if (s.empty() || s.size() > 16) return false;
            std::uint64_t value = 0;
            for (char c : s) {
                int digit;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else return false;
                value = (value << 4) | static_cast<std::uint64_t>(digit);
            }
            out = value;
            return true;
        }

    }  // namespace

    // ParserBase
    std::size_t ParserBase::feed(std::string_view data) {
        std::size_t i = 0;
        if (!data.empty()) saw_bytes_ = true;

        while (i < data.size() && status_ == ParseStatus::NeedMore) {
            switch (state_) {
                case State::StartLine:
                case State::Header:
                case State::ChunkSize:
                case State::ChunkDataEnd:
                case State::Trailer: {
                    auto nl = data.find('\n', i);
                    std::size_t end = (nl == std::string_view::npos) ? data.size() : nl;
                    line_.append(data.data() + i, end - i);
                    bool header_state = state_ == State::StartLine || state_ == State::Header ||
                                        state_ == State::Trailer;
                    if (header_state) {
                        header_bytes_ += end - i + (nl != std::string_view::npos ? 1 : 0);
                        if (header_bytes_ > limits_.max_header_bytes) {
                            fail(ParseError::HeadersTooLarge, "header section too large");
                            return end;
                        }
                    } else if (line_.size() > kMaxChunkLine) {
                        fail(ParseError::BadChunk, "chunk line too long");
                        return end;
                    }

                    if (nl == std::string_view::npos) {
                        i = data.size();
                        break;
                    }
                    i = nl + 1;
                    std::string_view line = line_;
                    if (line.empty() || line.back() != '\r') {
                        fail(ParseError::BadHeader, "HTTP lines must end with CRLF");
                        return end;
                    }
                    line.remove_suffix(1);
                    if (line.find('\r') != std::string_view::npos) {
                        fail(ParseError::BadHeader, "unexpected carriage return in HTTP line");
                        return end;
                    }
                    handle_line(line);
                    line_.clear();
                    break;
                }
                case State::FixedBody:
                case State::ChunkData: {
                    auto take = static_cast<std::size_t>(
                        std::min<std::uint64_t>(remaining_, data.size() - i));
                    if (!append_body(data.substr(i, take))) return i;
                    remaining_ -= take;
                    i += take;
                    if (remaining_ == 0) {
                        if (state_ == State::FixedBody) complete();
                        else state_ = State::ChunkDataEnd;
                    }
                    break;
                }
                case State::UntilClose: {
                    if (!append_body(data.substr(i))) return i;
                    i = data.size();
                    break;
                }
                case State::Done:
                    return i;
            }
        }
        return i;
    }

    void ParserBase::finish() {
        if (status_ != ParseStatus::NeedMore) return;
        if (state_ == State::UntilClose) {
            complete();
            return;
        }
        if (!saw_bytes_) {
            fail(ParseError::UnexpectedEof, "connection closed before any data");
        } else {
            fail(ParseError::UnexpectedEof, "connection closed mid-message");
        }
    }

    void ParserBase::handle_line(std::string_view line) {
        switch (state_) {
            case State::StartLine:
                // RFC 9112 §2.2: ignore at least one empty line before the start line.
                if (line.empty()) return;
                if (on_start_line(line)) state_ = State::Header;
                return;
            case State::Header:
                if (line.empty()) handle_headers_end();
                else handle_header_line(line);
                return;
            case State::ChunkSize:
                handle_chunk_size(line);
                return;
            case State::ChunkDataEnd:
                if (!line.empty()) {
                    fail(ParseError::BadChunk, "missing CRLF after chunk data");
                    return;
                }
                state_ = State::ChunkSize;
                return;
            case State::Trailer:
                if (line.empty()) {
                    complete();
                } else {
                    auto colon = line.find(':');
                    if (colon == std::string_view::npos ||
                        !util::is_token(line.substr(0, colon)) ||
                        !valid_field_value(line.substr(colon + 1))) {
                        fail(ParseError::BadHeader, "invalid trailer field");
                    }
                }
                return;
            default:
                return;
        }
    }

    void ParserBase::handle_header_line(std::string_view line) {
        if (util::is_ows(line.front())) {
            fail(ParseError::BadHeader, "obsolete line folding is not supported");
            return;
        }
        auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            fail(ParseError::BadHeader, "header line without ':'");
            return;
        }
        auto name = line.substr(0, colon);
        // RFC 9112 §5.1: no whitespace allowed between field name and colon.
        if (!util::is_token(name)) {
            fail(ParseError::BadHeader, "invalid header name");
            return;
        }
        if (headers_.size() >= limits_.max_header_count) {
            fail(ParseError::HeadersTooLarge, "too many header fields");
            return;
        }
        auto value = line.substr(colon + 1);
        if (!valid_field_value(value)) {
            fail(ParseError::BadHeader, "invalid control character in header value");
            return;
        }
        headers_.add(std::string(name), std::string(util::trim(value)));
    }

    void ParserBase::handle_headers_end() {
        BodySpec spec;
        switch (on_headers_complete(spec)) {
            case HeadersAction::Fail:
                return;  // derived class already called fail()
            case HeadersAction::Interim:
                headers_.clear();
                header_bytes_ = 0;
                state_ = State::StartLine;
                return;
            case HeadersAction::Continue:
                break;
        }
        headers_done_ = true;

        switch (spec.mode) {
            case BodyMode::None:
                framing_ = BodyFraming::None;
                complete();
                return;
            case BodyMode::Fixed:
                framing_ = BodyFraming::Length;
                if (!streaming_ && spec.length > limits_.max_body_bytes) {
                    fail(ParseError::BodyTooLarge, "body exceeds limit");
                    return;
                }
                if (spec.length == 0) {
                    complete();
                    return;
                }
                remaining_ = spec.length;
                state_ = State::FixedBody;
                return;
            case BodyMode::Chunked:
                framing_ = BodyFraming::Chunked;
                chunked_ = true;
                state_ = State::ChunkSize;
                return;
            case BodyMode::UntilClose:
                framing_ = BodyFraming::UntilClose;
                state_ = State::UntilClose;
                return;
        }
    }

    void ParserBase::handle_chunk_size(std::string_view line) {
        // chunk-size [ chunk-ext ] ; extensions are ignored.
        auto semi = line.find(';');
        auto size_str = util::trim(line.substr(0, semi));
        std::uint64_t size = 0;
        if (!parse_hex(size_str, size)) {
            fail(ParseError::BadChunk, "invalid chunk size");
            return;
        }
        if (size == 0) {
            state_ = State::Trailer;
            return;
        }
        if (!streaming_ && (size > limits_.max_body_bytes || body_.size() + size > limits_.max_body_bytes)) {
            fail(ParseError::BodyTooLarge, "body exceeds limit");
            return;
        }
        remaining_ = size;
        state_ = State::ChunkData;
    }

    bool ParserBase::append_body(std::string_view data) {
        if (!streaming_ && body_.size() + data.size() > limits_.max_body_bytes) {
            fail(ParseError::BodyTooLarge, "body exceeds limit");
            return false;
        }
        body_.append(data.data(), data.size());
        return true;
    }

    void ParserBase::complete() {
        state_ = State::Done;
        status_ = ParseStatus::Complete;
    }

    void ParserBase::fail(ParseError error, std::string message) {
        state_ = State::Done;
        status_ = ParseStatus::Error;
        error_ = error;
        error_message_ = std::move(message);
    }

    void ParserBase::reset_base() {
        headers_.clear();
        body_.clear();
        state_ = State::StartLine;
        status_ = ParseStatus::NeedMore;
        error_ = ParseError::None;
        error_message_.clear();
        line_.clear();
        header_bytes_ = 0;
        remaining_ = 0;
        headers_done_ = false;
        chunked_ = false;
        saw_bytes_ = false;
        framing_ = BodyFraming::None;
    }

    bool ParserBase::body_from_headers(BodySpec& spec, bool is_request) {
        // The proxy only implements the chunked transfer coding. Reject other
        // codings instead of silently exposing still-encoded bytes as the payload.
        if (headers_.contains("Transfer-Encoding")) {
            if (headers_.contains("Content-Length")) {
                fail(ParseError::BadTransferEncoding, "Transfer-Encoding with Content-Length is ambiguous");
                return false;
            }
            std::string transfer_encoding;
            bool first_transfer_encoding = true;
            for (const auto& field : headers_) {
                if (!util::iequals(field.name, "Transfer-Encoding")) continue;
                if (!first_transfer_encoding) transfer_encoding += ", ";
                transfer_encoding += field.value;
                first_transfer_encoding = false;
            }
            const auto transfer_status = parse_transfer_encoding(transfer_encoding);
            if (transfer_status == TransferEncodingStatus::Invalid) {
                fail(ParseError::BadTransferEncoding, "malformed Transfer-Encoding");
                return false;
            }
            if (transfer_status == TransferEncodingStatus::Unsupported) {
                fail(ParseError::UnsupportedTransferEncoding, "unsupported Transfer-Encoding");
                return false;
            }
            spec.mode = BodyMode::Chunked;
            return true;
        }

        if (headers_.contains("Content-Length")) {
            // Accept repeated identical values, but preserve empty list elements:
            // split_list() intentionally drops them and is too permissive here.
            std::uint64_t length = 0;
            bool first = true;
            for (const auto& field : headers_) {
                if (!util::iequals(field.name, "Content-Length")) continue;

                std::size_t start = 0;
                while (start <= field.value.size()) {
                    auto end = field.value.find(',', start);
                    if (end == std::string::npos) end = field.value.size();

                    auto value = util::trim(std::string_view(field.value).substr(start, end - start));
                    std::uint64_t n = 0;
                    if (!util::parse_uint(value, n) || (!first && n != length)) {
                        fail(ParseError::BadContentLength, "invalid Content-Length");
                        return false;
                    }
                    length = n;
                    first = false;

                    if (end == field.value.size()) break;
                    start = end + 1;
                }
            }
            if (first) {
                fail(ParseError::BadContentLength, "empty Content-Length");
                return false;
            }
            spec.mode = BodyMode::Fixed;
            spec.length = length;
            return true;
        }

        spec.mode = is_request ? BodyMode::None : BodyMode::UntilClose;
        return true;
    }

    // RequestParser
    bool RequestParser::on_start_line(std::string_view line) {
        // request-line = method SP request-target SP HTTP-version
        auto sp1 = line.find(' ');
        auto sp2 = (sp1 == std::string_view::npos) ? sp1 : line.find(' ', sp1 + 1);
        if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
            line.find(' ', sp2 + 1) != std::string_view::npos) {
            fail(ParseError::BadStartLine, "malformed request line");
            return false;
        }
        auto method = line.substr(0, sp1);
        auto target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        auto version = line.substr(sp2 + 1);

        bool invalid_target = target.empty();
        for (unsigned char c : target) {
            if (c <= 0x20 || c == 0x7f) invalid_target = true;
        }
        if (!util::is_token(method) || invalid_target) {
            fail(ParseError::BadStartLine, "malformed request line");
            return false;
        }
        if (!parse_version(version, version_) || version_.major != 1) {
            fail(ParseError::BadVersion, "unsupported HTTP version");
            return false;
        }
        method_ = std::string(method);
        target_ = std::string(target);
        return true;
    }

    ParserBase::HeadersAction RequestParser::on_headers_complete(BodySpec& spec) {
        std::size_t host_count = 0;
        for (const auto& field : headers_) {
            if (util::iequals(field.name, "Host")) ++host_count;
        }
        if (host_count > 1 || (version_.minor >= 1 && host_count != 1)) {
            fail(ParseError::BadHeader, host_count == 0 ? "missing Host header" : "multiple Host headers");
            return HeadersAction::Fail;
        }
        if (headers_.contains("Expect")) {
            const std::string expect = headers_.get_combined("Expect");
            if (version_.minor < 1 || !util::iequals(util::trim(expect), "100-continue")) {
                fail(ParseError::ExpectationFailed, "unsupported expectation");
                return HeadersAction::Fail;
            }
        }
        return body_from_headers(spec, true) ? HeadersAction::Continue : HeadersAction::Fail;
    }

    Request RequestParser::take() {
        Request req;
        req.method = std::move(method_);
        req.target = std::move(target_);
        req.version = version_;
        req.headers = std::move(headers_);
        req.body = std::move(body_);
        return req;
    }

    void RequestParser::reset() {
        reset_base();
        method_.clear();
        target_.clear();
        version_ = {};
    }

    // ResponseParser
    bool ResponseParser::on_start_line(std::string_view line) {
        // status-line = HTTP-version SP status-code SP [ reason-phrase ]
        if (line.size() < 12 || line[8] != ' ' || !parse_version(line.substr(0, 8), version_) ||
            version_.major != 1) {
            fail(ParseError::BadStartLine, "malformed status line");
            return false;
        }
        auto code = line.substr(9, 3);
        std::uint64_t status = 0;
        if (!util::parse_uint(code, status) || status < 100 || status > 999 ||
            (line.size() > 12 && line[12] != ' ')) {
            fail(ParseError::BadStartLine, "malformed status code");
            return false;
        }
        status_code_ = static_cast<int>(status);
        auto reason = line.size() > 13 ? line.substr(13) : std::string_view{};
        if (!valid_field_value(reason)) {
            fail(ParseError::BadStartLine, "invalid control character in reason phrase");
            return false;
        }
        reason_ = std::string(reason);
        return true;
    }

    ParserBase::HeadersAction ResponseParser::on_headers_complete(BodySpec& spec) {
        // Interim responses (100 Continue, 103 Early Hints...) are skipped.
        if (status_code_ >= 100 && status_code_ < 200 && status_code_ != 101) {
            return HeadersAction::Interim;
        }
        if (status_code_ == 205) {
            if (headers_.contains("Transfer-Encoding")) {
                fail(ParseError::BadTransferEncoding, "205 response must not contain a transfer coding");
                return HeadersAction::Fail;
            }
            if (headers_.contains("Content-Length")) {
                BodySpec framing;
                if (!body_from_headers(framing, false)) return HeadersAction::Fail;
                if (framing.mode != BodyMode::Fixed || framing.length != 0) {
                    fail(ParseError::BadContentLength, "205 response must have an empty body");
                    return HeadersAction::Fail;
                }
            }
            spec.mode = BodyMode::None;
            return HeadersAction::Continue;
        }
        if (response_has_no_body(request_method_, status_code_)) {
            spec.mode = BodyMode::None;
            return HeadersAction::Continue;
        }
        return body_from_headers(spec, false) ? HeadersAction::Continue : HeadersAction::Fail;
    }

    Response ResponseParser::take() {
        Response resp;
        resp.version = version_;
        resp.status = status_code_;
        resp.reason = std::move(reason_);
        resp.headers = std::move(headers_);
        resp.body = std::move(body_);
        return resp;
    }

    Response ResponseParser::take_head() {
        Response resp;
        resp.version = version_;
        resp.status = status_code_;
        resp.reason = reason_;
        resp.headers = std::move(headers_);
        headers_.clear();
        return resp;
    }

    void ResponseParser::reset(std::string request_method) {
        reset_base();
        request_method_ = std::move(request_method);
        version_ = {};
        status_code_ = 0;
        reason_.clear();
    }

}  // namespace proxy::http