#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "http/message.hpp"

namespace proxy::http {

    struct ParserLimits {
        std::size_t max_header_bytes = 64 * 1024;  // start line + all header lines
        std::size_t max_header_count = 100;
        std::size_t max_body_bytes = 8 * 1024 * 1024;
    };

    enum class ParseStatus { NeedMore, Complete, Error };

    enum class ParseError {
        None,
        BadStartLine,
        BadVersion,
        BadHeader,
        HeadersTooLarge,
        BadContentLength,
        BadTransferEncoding,
        UnsupportedTransferEncoding,
        ExpectationFailed,
        BadChunk,
        BodyTooLarge,
        UnexpectedEof,
    };

    // Incremental HTTP/1.1 message parser (RFC 9112).
    //
    // Bytes are pushed with feed(); the parser consumes only what belongs to the
    // current message so the caller can keep the rest (pipelined requests, or the
    // first bytes of a CONNECT tunnel). Chunked bodies are decoded, so after
    // completion `body` holds the payload and the message no longer depends on
    // Transfer-Encoding.
    class ParserBase {
    public:
        virtual ~ParserBase() = default;

        // Returns how many bytes of `data` were consumed.
        std::size_t feed(std::string_view data);
        // Signals that the peer closed the connection.
        void finish();

        ParseStatus status() const { return status_; }
        ParseError error() const { return error_; }
        const std::string& error_message() const { return error_message_; }
        bool headers_complete() const { return headers_done_; }
        const Headers& headers() const { return headers_; }
        bool was_chunked() const { return chunked_; }

    protected:
        enum class BodyMode { None, Fixed, Chunked, UntilClose };
        struct BodySpec {
            BodyMode mode = BodyMode::None;
            std::uint64_t length = 0;
        };
        enum class HeadersAction { Continue, Interim, Fail };

        explicit ParserBase(ParserLimits limits) : limits_(limits) {}

        virtual bool on_start_line(std::string_view line) = 0;
        // Decides how the body is framed. Return Interim to discard the message
        // (1xx responses) and parse the next one.
        virtual HeadersAction on_headers_complete(BodySpec& spec) = 0;

        void fail(ParseError error, std::string message);
        void reset_base();
        // Shared Content-Length / Transfer-Encoding logic. Only chunked is supported.
        bool body_from_headers(BodySpec& spec, bool is_request);

        Headers headers_;
        std::string body_;

    private:
        enum class State {
            StartLine,
            Header,
            FixedBody,
            ChunkSize,
            ChunkData,
            ChunkDataEnd,
            Trailer,
            UntilClose,
            Done,
        };

        void handle_line(std::string_view line);
        void handle_header_line(std::string_view line);
        void handle_headers_end();
        void handle_chunk_size(std::string_view line);
        void complete();
        bool append_body(std::string_view data);

        ParserLimits limits_;
        State state_ = State::StartLine;
        ParseStatus status_ = ParseStatus::NeedMore;
        ParseError error_ = ParseError::None;
        std::string error_message_;
        std::string line_;
        std::size_t header_bytes_ = 0;
        std::uint64_t remaining_ = 0;
        bool headers_done_ = false;
        bool chunked_ = false;
        bool saw_bytes_ = false;
    };

    class RequestParser : public ParserBase {
    public:
        explicit RequestParser(ParserLimits limits = {}) : ParserBase(limits) {}

        // Moves the parsed request out. Only valid when status() == Complete.
        Request take();
        // Prepares to parse the next message on the same connection.
        void reset();

        const std::string& method() const { return method_; }

    private:
        bool on_start_line(std::string_view line) override;
        HeadersAction on_headers_complete(BodySpec& spec) override;

        std::string method_;
        std::string target_;
        Version version_;
    };

    class ResponseParser : public ParserBase {
    public:
        // The request method matters: responses to HEAD never have a body.
        explicit ResponseParser(std::string request_method = "GET", ParserLimits limits = {})
            : ParserBase(limits), request_method_(std::move(request_method)) {}

        Response take();
        void reset(std::string request_method);

    private:
        bool on_start_line(std::string_view line) override;
        HeadersAction on_headers_complete(BodySpec& spec) override;

        std::string request_method_;
        Version version_;
        int status_code_ = 0;
        std::string reason_;
    };

}  // namespace proxy::http