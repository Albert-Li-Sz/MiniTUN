#include <minitun/ipc/local_client.hpp>

#include <array>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/local/stream_protocol.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include <minitun/common/secure_string.hpp>

#include "local_internal.hpp"

namespace minitun::ipc {
namespace {

constexpr std::size_t kClientReadBufferSize = 4096;

class ClientOperation final : public std::enable_shared_from_this<ClientOperation> {
  public:
    ClientOperation(const LocalClientOptions& options, common::Id expected_request_id,
                    std::vector<std::uint8_t> outbound)
        : options_(options), expected_request_id_(std::move(expected_request_id)),
          socket_(io_context_), deadline_(io_context_), decoder_(options.max_message_size),
          outbound_(std::move(outbound)) {}

    ~ClientOperation() noexcept { scrub_outbound(); }

    [[nodiscard]] common::Result<Response> run() {
        try {
            arm_connect_deadline();
            const asio::local::stream_protocol::endpoint endpoint{options_.socket_path};
            socket_.async_connect(endpoint, [self = shared_from_this()](const auto& error) {
                self->on_connect(error);
            });
            io_context_.run();
        } catch (const std::exception&) {
            complete(common::Error{common::ErrorCode::internal_error,
                                   "failed to start the IPC client operation"});
        } catch (...) {
            complete(common::Error{common::ErrorCode::internal_error,
                                   "failed to start the IPC client operation"});
        }

        // Async setup failures can leave a cancelled timer completion queued.
        // Drain it so its shared ownership is released before this call returns.
        try {
            io_context_.restart();
            static_cast<void>(io_context_.poll());
        } catch (...) {
        }

        if (!result_.has_value()) {
            return common::Result<Response>::failure(
                common::ErrorCode::internal_error, "IPC client operation ended without a response");
        }
        return std::move(*result_);
    }

  private:
    void arm_connect_deadline() {
        phase_ = Phase::connecting;
        deadline_.expires_after(options_.connect_timeout);
        deadline_.async_wait([self = shared_from_this()](const auto& error) {
            if (!error && self->phase_ == Phase::connecting) {
                self->complete(common::Error{common::ErrorCode::connection_timeout,
                                             "IPC socket connection timed out"});
            }
        });
    }

    void arm_request_deadline() {
        phase_ = Phase::requesting;
        deadline_.expires_after(options_.request_timeout);
        deadline_.async_wait([self = shared_from_this()](const auto& error) {
            if (!error && self->phase_ == Phase::requesting) {
                self->complete(
                    common::Error{common::ErrorCode::connection_timeout, "IPC request timed out"});
            }
        });
    }

    void on_connect(const asio::error_code& error) noexcept {
        try {
            if (is_complete()) {
                return;
            }
            if (error) {
                complete(detail::socket_error(error, "IPC socket connection"));
                return;
            }

            detail::cancel_timer(deadline_);
            arm_request_deadline();
            asio::async_write(socket_, asio::buffer(outbound_),
                              [self = shared_from_this()](const auto& write_error,
                                                          std::size_t /*bytes_written*/) {
                                  self->on_write(write_error);
                              });
        } catch (...) {
            complete(common::Error{common::ErrorCode::internal_error,
                                   "unexpected IPC client connection failure"});
        }
    }

    void on_write(const asio::error_code& error) noexcept {
        try {
            if (is_complete()) {
                return;
            }
            scrub_outbound();
            if (error) {
                complete(detail::socket_error(error, "IPC request write"));
                return;
            }
            read_response();
        } catch (...) {
            complete(common::Error{common::ErrorCode::internal_error,
                                   "unexpected IPC client write failure"});
        }
    }

    void read_response() {
        socket_.async_read_some(
            asio::buffer(read_buffer_),
            [self = shared_from_this()](const auto& error, std::size_t bytes_read) {
                self->on_read(error, bytes_read);
            });
    }

    void on_read(const asio::error_code& error, std::size_t bytes_read) noexcept {
        try {
            if (is_complete()) {
                return;
            }
            if (error) {
                if (error == asio::error::eof) {
                    const auto finished = decoder_.finish();
                    if (!finished) {
                        complete(finished.error());
                    } else {
                        complete(common::Error{common::ErrorCode::ipc_error,
                                               "IPC peer closed before responding"});
                    }
                } else {
                    complete(detail::socket_error(error, "IPC response read"));
                }
                return;
            }

            const auto bytes = std::span<const std::uint8_t>{read_buffer_.data(), bytes_read};
            auto decoded = decoder_.feed(bytes);
            if (!decoded) {
                complete(std::move(decoded).error());
                return;
            }
            if (decoded->empty()) {
                read_response();
                return;
            }
            if (decoded->size() != 1 || decoder_.buffered_size() != 0) {
                complete(common::Error{common::ErrorCode::protocol_error,
                                       "IPC peer sent an unexpected extra response"});
                return;
            }

            auto response = parse_response(decoded->front(), options_.max_message_size);
            if (!response) {
                common::Error response_error = std::move(response).error();
                if (response_error.code() == common::ErrorCode::invalid_argument) {
                    response_error = common::Error{common::ErrorCode::protocol_error,
                                                   "IPC peer returned a malformed response"};
                }
                complete(std::move(response_error));
                return;
            }
            if (response->request_id() != expected_request_id_) {
                complete(common::Error{common::ErrorCode::protocol_error,
                                       "IPC response request_id does not match the request"});
                return;
            }
            complete(common::Result<Response>{std::move(*response)});
        } catch (...) {
            complete(common::Error{common::ErrorCode::internal_error,
                                   "unexpected IPC client read failure"});
        }
    }

    void complete(common::Error error) noexcept {
        complete(common::Result<Response>{std::move(error)});
    }

    void complete(common::Result<Response> result) noexcept {
        if (is_complete()) {
            return;
        }
        try {
            result_.emplace(std::move(result));
            phase_ = Phase::complete;
            asio::error_code ignored;
            detail::cancel_timer(deadline_);
            socket_.cancel(ignored);
            socket_.close(ignored);
        } catch (...) {
            phase_ = Phase::complete;
            asio::error_code ignored;
            detail::cancel_timer(deadline_);
            socket_.close(ignored);
        }
    }

    [[nodiscard]] bool is_complete() const noexcept { return phase_ == Phase::complete; }

    void scrub_outbound() noexcept {
        common::secure_erase_memory(outbound_.data(), outbound_.size());
        outbound_.clear();
    }

    enum class Phase : std::uint8_t {
        idle,
        connecting,
        requesting,
        complete,
    };

    const LocalClientOptions& options_;
    common::Id expected_request_id_;
    asio::io_context io_context_;
    asio::local::stream_protocol::socket socket_;
    asio::steady_timer deadline_;
    FrameDecoder decoder_;
    std::vector<std::uint8_t> outbound_;
    std::array<std::uint8_t, kClientReadBufferSize> read_buffer_{};
    std::optional<common::Result<Response>> result_;
    Phase phase_{Phase::idle};
};

[[nodiscard]] common::Result<void> validate_options(const LocalClientOptions& options) {
    auto path = detail::validate_socket_path(options.socket_path);
    if (!path) {
        return path;
    }
    auto limits =
        detail::validate_transport_limits(options.max_message_size, options.connect_timeout);
    if (!limits) {
        return limits;
    }
    return detail::validate_transport_limits(options.max_message_size, options.request_timeout);
}

} // namespace

LocalClient::LocalClient(LocalClientOptions options) : options_(std::move(options)) {}

common::Result<Response> LocalClient::request(const Request& request) const {
    auto valid_options = validate_options(options_);
    if (!valid_options) {
        return valid_options.error();
    }

    auto payload = serialize_request(request, options_.max_message_size);
    if (!payload) {
        return std::move(payload).error();
    }
    auto frame = encode_frame(*payload, options_.max_message_size);
    common::secure_erase_memory(payload->data(), payload->size());
    if (!frame) {
        return std::move(frame).error();
    }

    try {
        auto operation =
            std::make_shared<ClientOperation>(options_, request.request_id, std::move(*frame));
        return operation->run();
    } catch (...) {
        return common::Result<Response>::failure(common::ErrorCode::internal_error,
                                                 "failed to allocate the IPC client operation");
    }
}

} // namespace minitun::ipc
