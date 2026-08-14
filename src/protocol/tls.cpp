#include <minitun/protocol/tls.hpp>

#include <array>
#include <cstdint>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <openssl/ssl.h>

namespace minitun::protocol {
namespace {

inline constexpr std::size_t kMaxTlsPathBytes = 4'096U;
inline constexpr std::size_t kMaxServerNameBytes = 253U;
inline constexpr std::size_t kMaxPipelinedFrames = 32U;
inline constexpr std::array<unsigned char, 10U> kSessionIdContext{
    'M', 'i', 'n', 'i', 'T', 'U', 'N', '-', 'v', '2'};

[[nodiscard]] common::Result<void> validate_tls_path(const std::string_view path,
                                                     const std::string_view description) {
    if (path.empty()) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             std::string{description} + " path is empty");
    }
    if (path.size() > kMaxTlsPathBytes || path.find('\0') != std::string_view::npos) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             std::string{description} + " path is invalid");
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> configure_context(asio::ssl::context& context) {
    try {
        context.set_options(asio::ssl::context::default_workarounds |
                            asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
                            asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1 |
                            asio::ssl::context::single_dh_use);
        SSL_CTX_set_options(context.native_handle(),
                            SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
        if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) != 1) {
            return common::Result<void>::failure(common::ErrorCode::tls_error,
                                                 "failed to enforce the minimum TLS version");
        }
        if (SSL_CTX_set_cipher_list(context.native_handle(), "HIGH:!aNULL:!MD5:!RC4:!3DES") !=
            1) {
            return common::Result<void>::failure(common::ErrorCode::tls_error,
                                                 "failed to configure TLS cipher policy");
        }
        return common::Result<void>::success();
    } catch (const std::exception&) {
        return common::Result<void>::failure(common::ErrorCode::tls_error,
                                             "failed to configure the TLS context");
    }
}

[[nodiscard]] common::Error transport_error(const asio::error_code& error,
                                            const std::string_view operation) {
    if (error == asio::error::operation_aborted) {
        return common::Error{common::ErrorCode::connection_timeout,
                             std::string{operation} + " was cancelled or timed out"};
    }
    if (error == asio::error::eof || error == asio::error::connection_reset ||
        error == asio::ssl::error::stream_truncated) {
        return common::Error{common::ErrorCode::connection_failed,
                             std::string{operation} + " ended because the peer closed"};
    }
    return common::Error{common::ErrorCode::connection_failed,
                         std::string{operation} + " failed"};
}

[[nodiscard]] std::uint32_t payload_length_from_header(
    const std::array<std::uint8_t, kFrameHeaderSize>& header) noexcept {
    return (static_cast<std::uint32_t>(header[12]) << 24U) |
           (static_cast<std::uint32_t>(header[13]) << 16U) |
           (static_cast<std::uint32_t>(header[14]) << 8U) |
           static_cast<std::uint32_t>(header[15]);
}

} // namespace

class TlsSessionCache::Impl final {
  public:
    ~Impl() noexcept {
        std::scoped_lock lock{mutex_};
        SSL_SESSION_free(session_);
    }

    [[nodiscard]] bool restore(TlsStream& stream) noexcept {
        std::scoped_lock lock{mutex_};
        return session_ != nullptr && SSL_set_session(stream.native_handle(), session_) == 1;
    }

    [[nodiscard]] bool capture(TlsStream& stream) noexcept {
        SSL_SESSION* replacement = SSL_get1_session(stream.native_handle());
        if (replacement == nullptr) {
            return false;
        }
        if (SSL_SESSION_is_resumable(replacement) != 1) {
            SSL_SESSION_free(replacement);
            return false;
        }
        std::scoped_lock lock{mutex_};
        SSL_SESSION* previous = std::exchange(session_, replacement);
        SSL_SESSION_free(previous);
        return true;
    }

  private:
    std::mutex mutex_;
    SSL_SESSION* session_{nullptr};
};

TlsSessionCache::TlsSessionCache() : implementation_(std::make_unique<Impl>()) {}

TlsSessionCache::~TlsSessionCache() noexcept = default;

bool TlsSessionCache::restore(TlsStream& stream) noexcept {
    return implementation_->restore(stream);
}

bool TlsSessionCache::capture(TlsStream& stream) noexcept {
    return implementation_->capture(stream);
}

common::Result<std::shared_ptr<asio::ssl::context>>
make_server_tls_context(const ServerTlsContextOptions& options) {
    if (auto result = validate_tls_path(options.certificate_chain_path, "TLS certificate");
        !result) {
        return common::Result<std::shared_ptr<asio::ssl::context>>::failure(result.error());
    }
    if (auto result = validate_tls_path(options.private_key_path, "TLS private key"); !result) {
        return common::Result<std::shared_ptr<asio::ssl::context>>::failure(result.error());
    }
    if (!options.client_ca_certificate_path.empty()) {
        auto result = validate_tls_path(options.client_ca_certificate_path, "client TLS CA");
        if (!result) {
            return common::Result<std::shared_ptr<asio::ssl::context>>::failure(result.error());
        }
    }

    try {
        auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
        auto configured = configure_context(*context);
        if (!configured) {
            return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
                configured.error());
        }
        context->use_certificate_chain_file(options.certificate_chain_path);
        context->use_private_key_file(options.private_key_path, asio::ssl::context::pem);
        if (SSL_CTX_check_private_key(context->native_handle()) != 1) {
            return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
                common::ErrorCode::tls_error,
                "TLS certificate and private key do not match");
        }
        if (!options.client_ca_certificate_path.empty()) {
            context->load_verify_file(options.client_ca_certificate_path);
            context->set_verify_mode(asio::ssl::verify_peer);
            SSL_CTX_set_verify_depth(context->native_handle(), 8);
        }
        SSL_CTX_set_session_cache_mode(context->native_handle(), SSL_SESS_CACHE_SERVER);
        SSL_CTX_set_timeout(context->native_handle(), 300L);
        if (SSL_CTX_set_session_id_context(context->native_handle(), kSessionIdContext.data(),
                                           static_cast<unsigned int>(kSessionIdContext.size())) !=
            1) {
            return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
                common::ErrorCode::tls_error, "failed to configure TLS session resumption");
        }
#if defined(TLS1_3_VERSION)
        if (SSL_CTX_set_num_tickets(context->native_handle(), 2U) != 1) {
            return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
                common::ErrorCode::tls_error, "failed to configure TLS session tickets");
        }
#endif
        return context;
    } catch (const std::exception&) {
        return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
            common::ErrorCode::tls_error,
            "failed to load the TLS certificate or private key");
    }
}

common::Result<std::shared_ptr<asio::ssl::context>>
make_client_tls_context(const ClientTlsContextOptions& options) {
    if (!options.ca_certificate_path.empty() && !options.ca_certificate_pem.empty()) {
        return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
            common::ErrorCode::invalid_argument,
            "TLS CA path and inline certificate cannot both be configured");
    }
    if (options.client_certificate_pem.empty() != options.client_private_key_pem.empty()) {
        return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
            common::ErrorCode::invalid_argument,
            "TLS client certificate and private key must be configured together");
    }
    if (!options.ca_certificate_path.empty()) {
        auto valid = validate_tls_path(options.ca_certificate_path, "TLS CA certificate");
        if (!valid) {
            return common::Result<std::shared_ptr<asio::ssl::context>>::failure(valid.error());
        }
    }

    try {
        auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
        auto configured = configure_context(*context);
        if (!configured) {
            return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
                configured.error());
        }
        if (!options.ca_certificate_pem.empty()) {
            context->add_certificate_authority(
                asio::buffer(options.ca_certificate_pem.data(), options.ca_certificate_pem.size()));
        } else if (options.ca_certificate_path.empty()) {
            context->set_default_verify_paths();
        } else {
            context->load_verify_file(std::string{options.ca_certificate_path});
        }
        if (!options.client_certificate_pem.empty()) {
            context->use_certificate_chain(
                asio::buffer(options.client_certificate_pem.data(),
                             options.client_certificate_pem.size()));
            context->use_private_key(
                asio::buffer(options.client_private_key_pem.data(),
                             options.client_private_key_pem.size()),
                asio::ssl::context::pem);
            if (SSL_CTX_check_private_key(context->native_handle()) != 1) {
                return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
                    common::ErrorCode::tls_error,
                    "TLS client certificate and private key do not match");
            }
        }
        SSL_CTX_set_session_cache_mode(context->native_handle(), SSL_SESS_CACHE_CLIENT);
        return context;
    } catch (const std::exception&) {
        return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
            common::ErrorCode::tls_error,
            "failed to load trusted TLS certificates");
    }
}

common::Result<void> configure_client_tls_stream(TlsStream& stream,
                                                 const std::string& server_name,
                                                 const bool insecure_skip_verify) {
    if (server_name.empty() || server_name.size() > kMaxServerNameBytes ||
        server_name.find('\0') != std::string::npos) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "TLS server name is invalid");
    }
    try {
        if (SSL_set_tlsext_host_name(stream.native_handle(), server_name.c_str()) != 1) {
            return common::Result<void>::failure(common::ErrorCode::tls_error,
                                                 "failed to configure TLS SNI");
        }
        if (insecure_skip_verify) {
            stream.set_verify_mode(asio::ssl::verify_none);
        } else {
            stream.set_verify_mode(asio::ssl::verify_peer);
            stream.set_verify_callback(asio::ssl::host_name_verification(server_name));
        }
        return common::Result<void>::success();
    } catch (const std::exception&) {
        return common::Result<void>::failure(common::ErrorCode::tls_error,
                                             "failed to configure TLS peer verification");
    }
}

bool tls_session_reused(TlsStream& stream) noexcept {
    return SSL_session_reused(stream.native_handle()) == 1;
}

asio::awaitable<common::Result<Frame>> async_read_frame(TlsStream& stream,
                                                        const std::size_t max_frame_size) {
    std::array<std::uint8_t, kFrameHeaderSize> header{};
    asio::error_code error;
    const std::size_t header_bytes = co_await asio::async_read(
        stream, asio::buffer(header), asio::redirect_error(asio::use_awaitable, error));
    if (error || header_bytes != header.size()) {
        co_return common::Result<Frame>::failure(transport_error(error, "remote frame read"));
    }

    FrameDecoder decoder{max_frame_size};
    auto header_result = decoder.feed(header);
    if (!header_result) {
        co_return common::Result<Frame>::failure(header_result.error());
    }
    if (!header_result->empty()) {
        if (header_result->size() != 1U) {
            co_return common::Result<Frame>::failure(common::ErrorCode::internal_error,
                                                     "remote frame decoder returned extra frames");
        }
        co_return std::move(header_result->front());
    }

    const std::size_t payload_size = payload_length_from_header(header);
    try {
        std::vector<std::uint8_t> payload(payload_size);
        const std::size_t payload_bytes = co_await asio::async_read(
            stream, asio::buffer(payload), asio::redirect_error(asio::use_awaitable, error));
        if (error || payload_bytes != payload.size()) {
            co_return common::Result<Frame>::failure(
                transport_error(error, "remote frame payload read"));
        }
        auto payload_result = decoder.feed(payload);
        if (!payload_result) {
            co_return common::Result<Frame>::failure(payload_result.error());
        }
        if (payload_result->size() != 1U || !decoder.finish()) {
            co_return common::Result<Frame>::failure(common::ErrorCode::internal_error,
                                                     "remote frame decoder did not finish a frame");
        }
        co_return std::move(payload_result->front());
    } catch (const std::bad_alloc&) {
        co_return common::Result<Frame>::failure(common::ErrorCode::resource_exhausted,
                                                 "insufficient memory while reading a remote frame");
    } catch (const std::length_error&) {
        co_return common::Result<Frame>::failure(common::ErrorCode::resource_exhausted,
                                                 "insufficient memory while reading a remote frame");
    }
}

asio::awaitable<common::Result<void>> async_write_frame(TlsStream& stream, Frame frame,
                                                        const std::size_t max_frame_size) {
    auto encoded = encode_frame(frame, max_frame_size);
    if (!encoded) {
        co_return common::Result<void>::failure(encoded.error());
    }

    asio::error_code error;
    const std::size_t written = co_await asio::async_write(
        stream, asio::buffer(*encoded), asio::redirect_error(asio::use_awaitable, error));
    if (error || written != encoded->size()) {
        co_return common::Result<void>::failure(transport_error(error, "remote frame write"));
    }
    co_return common::Result<void>::success();
}

asio::awaitable<common::Result<void>> async_write_frames(TlsStream& stream,
                                                         std::vector<Frame> frames,
                                                         const std::size_t max_frame_size) {
    if (frames.empty() || frames.size() > kMaxPipelinedFrames) {
        co_return common::Result<void>::failure(
            common::ErrorCode::invalid_argument,
            "remote frame pipeline must contain between 1 and 32 frames");
    }

    try {
        std::vector<std::uint8_t> batch;
        for (const auto& frame : frames) {
            auto encoded = encode_frame(frame, max_frame_size);
            if (!encoded) {
                co_return common::Result<void>::failure(encoded.error());
            }
            if (batch.size() > (kMaxFrameSize * kMaxPipelinedFrames) - encoded->size()) {
                co_return common::Result<void>::failure(
                    common::ErrorCode::frame_too_large,
                    "remote frame pipeline exceeds the bounded window size");
            }
            batch.insert(batch.end(), encoded->begin(), encoded->end());
        }

        asio::error_code error;
        const std::size_t written = co_await asio::async_write(
            stream, asio::buffer(batch), asio::redirect_error(asio::use_awaitable, error));
        if (error || written != batch.size()) {
            co_return common::Result<void>::failure(
                transport_error(error, "remote frame pipeline write"));
        }
        co_return common::Result<void>::success();
    } catch (const std::bad_alloc&) {
        co_return common::Result<void>::failure(
            common::ErrorCode::resource_exhausted,
            "insufficient memory while encoding a remote frame pipeline");
    } catch (const std::length_error&) {
        co_return common::Result<void>::failure(
            common::ErrorCode::resource_exhausted,
            "insufficient memory while encoding a remote frame pipeline");
    }
}

void close_tls_stream(TlsStream& stream) noexcept {
    try {
        asio::error_code ignored;
        stream.lowest_layer().cancel(ignored);
        stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        stream.lowest_layer().close(ignored);
    } catch (...) {
    }
}

} // namespace minitun::protocol
