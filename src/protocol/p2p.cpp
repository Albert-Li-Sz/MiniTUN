#include <minitun/protocol/p2p.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/auth.hpp>
#include <minitun/protocol/relay.hpp>

namespace minitun::protocol {
namespace {

inline constexpr std::array<std::uint8_t, 4U> kOfferMagic{'M', 'T', 'P', '2'};
inline constexpr std::array<std::uint8_t, 4U> kDirectMagic{'M', 'T', 'P', 'D'};
inline constexpr std::array<std::uint8_t, 4U> kFallbackMagic{'M', 'T', 'F', 'B'};
inline constexpr std::array<std::uint8_t, 4U> kReadyMagic{'M', 'T', 'O', 'K'};
inline constexpr std::uint8_t kP2pVersion = 1U;
inline constexpr std::uint8_t kAddressV4 = 4U;
inline constexpr std::uint8_t kAddressV6 = 6U;
inline constexpr std::size_t kOfferHeaderSize = 8U;
inline constexpr std::size_t kDirectHandshakeSize = kDirectMagic.size() + kAuthenticationNonceSize;
inline constexpr std::chrono::hours kMaximumTimeout{24};

[[nodiscard]] bool valid_timeout(const std::chrono::seconds value) noexcept {
    return value > std::chrono::seconds::zero() && value <= kMaximumTimeout;
}

void close_socket(asio::ip::tcp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

// The direct path upgrades to TLS 1.3 with the one-time rendezvous token as an
// external PSK, so the raw candidate socket never carries application data in
// the clear. The identity string is the only information sent in plaintext.
inline constexpr std::string_view kDirectPskIdentity{"minitun-p2p-direct-v1"};
[[nodiscard]] int direct_psk_ex_index() noexcept {
    // asio::ssl stores its own verify callback in SSL app-data, so the token
    // travels through a dedicated ex-data slot instead.
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}


[[nodiscard]] SSL_SESSION* make_psk_session(SSL* ssl, const AuthenticationNonce& token) {
    auto* session = SSL_SESSION_new();
    if (session == nullptr) {
        return nullptr;
    }
    // For TLS 1.3 the session acts purely as a PSK carrier: the protocol
    // version selects TLS 1.3, the cipher field names a TLS 1.3 ciphersuite,
    // and the master key field carries the PSK bytes.
    constexpr std::array<unsigned char, 2U> kAes256GcmSha384Id{0x13U, 0x02U};
    const SSL_CIPHER* const cipher = SSL_CIPHER_find(ssl, kAes256GcmSha384Id.data());
    if (cipher == nullptr ||
        SSL_SESSION_set_protocol_version(session, TLS1_3_VERSION) != 1 ||
        SSL_SESSION_set_cipher(session, cipher) != 1 ||
        SSL_SESSION_set1_master_key(session, token.data(), token.size()) != 1) {
        SSL_SESSION_free(session);
        return nullptr;
    }
    return session;
}

// OpenSSL 3.5 removed the EVP_MD parameter from the server-side PSK lookup
// callback; keep both signatures working across supported OpenSSL versions.
#if OPENSSL_VERSION_NUMBER >= 0x30500000L
int find_direct_psk(SSL* ssl, const unsigned char* identity,
                    const std::size_t identity_len, SSL_SESSION** session) {
#else
int find_direct_psk(SSL* ssl, const EVP_MD* /*md*/, const unsigned char* identity,
                    const std::size_t identity_len, SSL_SESSION** session) {
#endif
    const auto* token =
        static_cast<const AuthenticationNonce*>(SSL_get_ex_data(ssl, direct_psk_ex_index()));
    if (token == nullptr || identity_len != kDirectPskIdentity.size() ||
        !std::equal(kDirectPskIdentity.begin(), kDirectPskIdentity.end(), identity)) {
        return 0;
    }
    auto* candidate = make_psk_session(ssl, *token);
    if (candidate == nullptr) {
        return 0;
    }
    *session = candidate;
    return 1;
}

int use_direct_psk(SSL* ssl, const EVP_MD* /*md*/, const unsigned char** identity,
                   std::size_t* identity_len, SSL_SESSION** session) {
    const auto* token =
        static_cast<const AuthenticationNonce*>(SSL_get_ex_data(ssl, direct_psk_ex_index()));
    if (token == nullptr) {
        return 0;
    }
    auto* candidate = make_psk_session(ssl, *token);
    if (candidate == nullptr) {
        return 0;
    }
    *identity = reinterpret_cast<const unsigned char*>(kDirectPskIdentity.data());
    *identity_len = kDirectPskIdentity.size();
    *session = candidate;
    return 1;
}

void configure_direct_tls(TlsStream& stream, AuthenticationNonce& token, const bool server) {
    auto* ssl = stream.native_handle();
    SSL_set_ex_data(ssl, direct_psk_ex_index(), &token);
    if (server) {
        SSL_set_psk_find_session_callback(ssl, find_direct_psk);
    } else {
        SSL_set_psk_use_session_callback(ssl, use_direct_psk);
    }
}

void shutdown_send(asio::ip::tcp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

template <typename Stream>
[[nodiscard]] asio::awaitable<bool> read_exact(Stream& stream, void* data, const std::size_t size) {
    asio::error_code error;
    const std::size_t read = co_await asio::async_read(
        stream, asio::buffer(data, size), asio::redirect_error(asio::use_awaitable, error));
    co_return !error && read == size;
}

template <typename Stream>
[[nodiscard]] asio::awaitable<bool> write_exact(Stream& stream, const void* data,
                                                const std::size_t size) {
    asio::error_code error;
    const std::size_t written = co_await asio::async_write(
        stream, asio::buffer(data, size), asio::redirect_error(asio::use_awaitable, error));
    co_return !error && written == size;
}

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
make_offer(const asio::ip::address& address, const std::uint16_t port,
           const AuthenticationNonce& token) {
    if (address.is_unspecified() || port == 0U) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::invalid_argument, "P2P direct candidate is invalid");
    }
    try {
        std::vector<std::uint8_t> result;
        result.reserve(kOfferHeaderSize + 16U + token.size());
        result.insert(result.end(), kOfferMagic.begin(), kOfferMagic.end());
        result.push_back(kP2pVersion);
        result.push_back(address.is_v4() ? kAddressV4 : kAddressV6);
        result.push_back(static_cast<std::uint8_t>((port >> 8U) & 0xffU));
        result.push_back(static_cast<std::uint8_t>(port & 0xffU));
        if (address.is_v4()) {
            const auto bytes = address.to_v4().to_bytes();
            result.insert(result.end(), bytes.begin(), bytes.end());
        } else {
            const auto bytes = address.to_v6().to_bytes();
            result.insert(result.end(), bytes.begin(), bytes.end());
        }
        result.insert(result.end(), token.begin(), token.end());
        return result;
    } catch (...) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::resource_exhausted, "P2P offer could not be allocated");
    }
}

struct ParsedOffer final {
    asio::ip::tcp::endpoint candidate;
    AuthenticationNonce token{};
};

[[nodiscard]] asio::awaitable<common::Result<ParsedOffer>>
read_offer(asio::ip::tcp::socket& socket) {
    std::array<std::uint8_t, kOfferHeaderSize> header{};
    if (!co_await read_exact(socket, header.data(), header.size()) ||
        !std::equal(kOfferMagic.begin(), kOfferMagic.end(), header.begin()) ||
        header[4] != kP2pVersion || (header[5] != kAddressV4 && header[5] != kAddressV6)) {
        co_return common::Result<ParsedOffer>::failure(common::ErrorCode::protocol_error,
                                                       "P2P offer header is invalid");
    }
    const std::uint16_t port =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(header[6]) << 8U) | header[7]);
    if (port == 0U) {
        co_return common::Result<ParsedOffer>::failure(common::ErrorCode::protocol_error,
                                                       "P2P offer contains a zero port");
    }
    const std::size_t address_size = header[5] == kAddressV4 ? 4U : 16U;
    std::array<std::uint8_t, 16U> address_bytes{};
    ParsedOffer result;
    if (!co_await read_exact(socket, address_bytes.data(), address_size) ||
        !co_await read_exact(socket, result.token.data(), result.token.size())) {
        co_return common::Result<ParsedOffer>::failure(common::ErrorCode::protocol_error,
                                                       "P2P offer is truncated");
    }
    if (header[5] == kAddressV4) {
        asio::ip::address_v4::bytes_type bytes{};
        std::copy_n(address_bytes.begin(), bytes.size(), bytes.begin());
        result.candidate = asio::ip::tcp::endpoint{asio::ip::address_v4{bytes}, port};
    } else {
        asio::ip::address_v6::bytes_type bytes{};
        std::copy_n(address_bytes.begin(), bytes.size(), bytes.begin());
        result.candidate = asio::ip::tcp::endpoint{asio::ip::address_v6{bytes}, port};
    }
    if (result.candidate.address().is_unspecified()) {
        co_return common::Result<ParsedOffer>::failure(common::ErrorCode::protocol_error,
                                                       "P2P offer candidate is unspecified");
    }
    co_return result;
}

class HostRace final : public std::enable_shared_from_this<HostRace> {
  public:
    HostRace(TlsStream& relay_stream, asio::ip::tcp::acceptor acceptor, AuthenticationNonce token,
             const std::chrono::seconds timeout)
        : relay_stream_(relay_stream), acceptor_(std::move(acceptor)), token_(token),
          timer_(relay_stream.get_executor()), completion_(relay_stream.get_executor()),
          timeout_(timeout) {}

    [[nodiscard]] asio::awaitable<common::Result<P2pHostUpgrade>> run() {
        completion_.expires_at(std::chrono::steady_clock::time_point::max());
        timer_.expires_after(timeout_);
        auto self = shared_from_this();
        timer_.async_wait([self](const asio::error_code& error) {
            if (!error) {
                self->fail(common::ErrorCode::connection_timeout, "P2P negotiation timed out");
            }
        });
        read_fallback();
        accept_direct();
        asio::error_code ignored;
        co_await completion_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        if (failure_.has_value()) {
            co_return common::Result<P2pHostUpgrade>::failure(std::move(*failure_));
        }
        co_return P2pHostUpgrade{path_, std::move(direct_stream_)};
    }

  private:
    void read_fallback() {
        auto self = shared_from_this();
        asio::async_read(relay_stream_, asio::buffer(fallback_),
                         [self](const asio::error_code& error, const std::size_t bytes) {
                             if (self->done_) {
                                 return;
                             }
                             if (error || bytes != self->fallback_.size() ||
                                 self->fallback_ != kFallbackMagic) {
                                 self->fail(common::ErrorCode::protocol_error,
                                            "P2P peer sent an invalid fallback request");
                                 return;
                             }
                             self->path_ = P2pPath::relay;
                             self->finish(false);
                         });
    }

    void accept_direct() {
        if (done_) {
            return;
        }
        auto self = shared_from_this();
        acceptor_.async_accept([self](const asio::error_code& error,
                                      asio::ip::tcp::socket socket) mutable {
            if (self->done_) {
                close_socket(socket);
                return;
            }
            if (error) {
                if (error != asio::error::operation_aborted) {
                    self->fail(common::ErrorCode::connection_failed, "P2P direct listener failed");
                }
                return;
            }
            auto candidate = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
            self->pending_direct_ = candidate;
            auto handshake = std::make_shared<std::array<std::uint8_t, kDirectHandshakeSize>>();
            asio::async_read(*candidate, asio::buffer(*handshake),
                             [self, candidate, handshake](const asio::error_code& read_error,
                                                          const std::size_t bytes) {
                                 if (self->done_) {
                                     close_socket(*candidate);
                                     return;
                                 }
                                 const bool valid =
                                     !read_error && bytes == handshake->size() &&
                                     std::equal(kDirectMagic.begin(), kDirectMagic.end(),
                                                handshake->begin()) &&
                                     CRYPTO_memcmp(handshake->data() + kDirectMagic.size(),
                                                   self->token_.data(), self->token_.size()) == 0;
                                 if (!valid) {
                                     close_socket(*candidate);
                                     if (self->pending_direct_ == candidate) {
                                         self->pending_direct_.reset();
                                     }
                                     self->accept_direct();
                                     return;
                                 }
                                 auto stream = std::make_shared<TlsStream>(
                                     std::move(*candidate), self->direct_tls_context_);
                                 configure_direct_tls(*stream, self->token_, true);
                                 stream->async_handshake(
                                     asio::ssl::stream_base::server,
                                     [self, stream](const asio::error_code& handshake_error) {
                                         if (self->done_) {
                                             close_tls_stream(*stream);
                                             return;
                                         }
                                         if (handshake_error) {
                                             close_tls_stream(*stream);
                                             self->pending_direct_.reset();
                                             self->accept_direct();
                                             return;
                                         }
                                         self->path_ = P2pPath::direct;
                                         self->direct_stream_ =
                                             std::make_unique<TlsStream>(std::move(*stream));
                                         self->pending_direct_.reset();
                                         self->finish(true);
                                     });
                             });
        });
    }

    void fail(const common::ErrorCode code, const char* message) {
        if (done_) {
            return;
        }
        failure_.emplace(code, message);
        finish(true);
    }

    void finish(const bool close_relay) noexcept {
        if (done_) {
            return;
        }
        done_ = true;
        asio::error_code ignored;
        acceptor_.cancel(ignored);
        acceptor_.close(ignored);
        if (pending_direct_ != nullptr) {
            close_socket(*pending_direct_);
            pending_direct_.reset();
        }
        try {
            static_cast<void>(timer_.cancel());
        } catch (...) {
        }
        if (close_relay) {
            close_tls_stream(relay_stream_);
        }
        try {
            static_cast<void>(completion_.cancel());
        } catch (...) {
        }
    }

    TlsStream& relay_stream_;
    asio::ip::tcp::acceptor acceptor_;
    AuthenticationNonce token_{};
    asio::steady_timer timer_;
    asio::steady_timer completion_;
    std::chrono::seconds timeout_;
    std::array<std::uint8_t, kFallbackMagic.size()> fallback_{};
    asio::ssl::context direct_tls_context_{asio::ssl::context::tlsv13_server};
    std::unique_ptr<TlsStream> direct_stream_;
    std::shared_ptr<asio::ip::tcp::socket> pending_direct_;
    std::optional<common::Error> failure_;
    P2pPath path_{P2pPath::relay};
    bool done_{false};
};

class TcpRelayOperation final : public std::enable_shared_from_this<TcpRelayOperation> {
  public:
    TcpRelayOperation(asio::ip::tcp::socket& first, asio::ip::tcp::socket& second,
                      const std::chrono::seconds timeout)
        : first_(first), second_(second), timeout_(timeout), activity_(first.get_executor()),
          completion_(first.get_executor()), started_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] asio::awaitable<common::Result<P2pRelayStats>> run() {
        activity_.expires_after(timeout_);
        completion_.expires_at(std::chrono::steady_clock::time_point::max());
        auto self = shared_from_this();
        asio::co_spawn(first_.get_executor(), pump(first_, second_, first_buffer_, true),
                       [self](const std::exception_ptr& failure) {
                           if (failure) {
                               self->finish_direction(
                                   common::Error{common::ErrorCode::internal_error,
                                                 "P2P relay direction failed unexpectedly"});
                           }
                       });
        asio::co_spawn(first_.get_executor(), pump(second_, first_, second_buffer_, false),
                       [self](const std::exception_ptr& failure) {
                           if (failure) {
                               self->finish_direction(
                                   common::Error{common::ErrorCode::internal_error,
                                                 "P2P relay direction failed unexpectedly"});
                           }
                       });
        asio::co_spawn(
            first_.get_executor(), watch_inactivity(), [self](const std::exception_ptr& failure) {
                if (failure) {
                    self->set_failure(common::Error{common::ErrorCode::internal_error,
                                                    "P2P relay timer failed unexpectedly"});
                    self->close_both();
                    self->complete();
                }
            });
        asio::error_code ignored;
        co_await completion_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        done_ = true;
        cancel(activity_);
        stats_.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_);
        if (failure_.has_value()) {
            co_return common::Result<P2pRelayStats>::failure(std::move(*failure_));
        }
        co_return stats_;
    }

  private:
    [[nodiscard]] asio::awaitable<void> pump(asio::ip::tcp::socket& source,
                                             asio::ip::tcp::socket& destination,
                                             std::array<std::uint8_t, kRelayBufferSize>& buffer,
                                             const bool first_direction) {
        for (;;) {
            asio::error_code read_error;
            const std::size_t bytes = co_await source.async_read_some(
                asio::buffer(buffer), asio::redirect_error(asio::use_awaitable, read_error));
            if (bytes != 0U) {
                asio::error_code write_error;
                const std::size_t written = co_await asio::async_write(
                    destination, asio::buffer(buffer.data(), bytes),
                    asio::redirect_error(asio::use_awaitable, write_error));
                if (write_error || written != bytes) {
                    finish_error(write_error, "P2P relay write failed");
                    co_return;
                }
                if (first_direction) {
                    stats_.first_to_second_bytes += static_cast<std::uint64_t>(written);
                } else {
                    stats_.second_to_first_bytes += static_cast<std::uint64_t>(written);
                }
                note_activity();
            }
            if (read_error) {
                if (read_error == asio::error::eof) {
                    shutdown_send(destination);
                    finish_direction();
                } else if (normal_disconnect(read_error)) {
                    close_both();
                    finish_direction();
                } else {
                    finish_direction(common::Error{common::ErrorCode::connection_failed,
                                                   "P2P relay read failed"});
                }
                co_return;
            }
        }
    }

    [[nodiscard]] asio::awaitable<void> watch_inactivity() {
        while (!done_) {
            asio::error_code error;
            co_await activity_.async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (done_) {
                co_return;
            }
            if (error == asio::error::operation_aborted) {
                continue;
            }
            set_failure(common::Error{error ? common::ErrorCode::internal_error
                                            : common::ErrorCode::connection_timeout,
                                      error ? "P2P relay inactivity timer failed"
                                            : "P2P relay inactivity timeout expired"});
            close_both();
            complete();
            co_return;
        }
    }

    static bool normal_disconnect(const asio::error_code& error) noexcept {
        return error == asio::error::connection_reset || error == asio::error::broken_pipe ||
               error == asio::error::operation_aborted;
    }

    void finish_error(const asio::error_code& error, const char* message) {
        if (normal_disconnect(error)) {
            close_both();
            finish_direction();
            return;
        }
        finish_direction(common::Error{common::ErrorCode::connection_failed, message});
    }

    void finish_direction(std::optional<common::Error> failure = std::nullopt) {
        if (done_) {
            return;
        }
        if (failure.has_value()) {
            set_failure(std::move(*failure));
            close_both();
        }
        ++finished_directions_;
        if (finished_directions_ >= 2U || failure.has_value()) {
            complete();
        }
    }

    void complete() noexcept {
        if (done_) {
            return;
        }
        done_ = true;
        cancel(activity_);
        cancel(completion_);
    }

    void set_failure(common::Error error) {
        if (!failure_.has_value()) {
            failure_ = std::move(error);
        }
    }

    void note_activity() {
        if (!done_) {
            activity_.expires_after(timeout_);
        }
    }

    void close_both() noexcept {
        close_socket(first_);
        close_socket(second_);
    }

    static void cancel(asio::steady_timer& timer) noexcept {
        try {
            static_cast<void>(timer.cancel());
        } catch (...) {
        }
    }

    asio::ip::tcp::socket& first_;
    asio::ip::tcp::socket& second_;
    std::chrono::seconds timeout_;
    asio::steady_timer activity_;
    asio::steady_timer completion_;
    std::array<std::uint8_t, kRelayBufferSize> first_buffer_{};
    std::array<std::uint8_t, kRelayBufferSize> second_buffer_{};
    P2pRelayStats stats_;
    std::optional<common::Error> failure_;
    std::chrono::steady_clock::time_point started_;
    std::size_t finished_directions_{0U};
    bool done_{false};
};

} // namespace

asio::awaitable<common::Result<P2pHostUpgrade>>
accept_p2p_upgrade(TlsStream& relay_stream, const asio::ip::address& candidate_address,
                   const std::chrono::seconds negotiation_timeout) {
    if (!valid_timeout(negotiation_timeout) || candidate_address.is_unspecified()) {
        co_return common::Result<P2pHostUpgrade>::failure(common::ErrorCode::invalid_argument,
                                                          "P2P host options are invalid");
    }
    asio::error_code error;
    asio::ip::tcp::acceptor acceptor{relay_stream.get_executor()};
    const asio::ip::tcp::endpoint endpoint{candidate_address, 0U};
    acceptor.open(endpoint.protocol(), error);
    if (!error) {
        acceptor.set_option(asio::socket_base::reuse_address{true}, error);
    }
    if (!error) {
        acceptor.bind(endpoint, error);
    }
    if (!error) {
        acceptor.listen(4, error);
    }
    if (error) {
        co_return common::Result<P2pHostUpgrade>::failure(
            common::ErrorCode::connection_failed, "P2P direct listener could not be created");
    }
    auto token = generate_authentication_nonce();
    if (!token) {
        co_return common::Result<P2pHostUpgrade>::failure(token.error());
    }
    auto offer = make_offer(candidate_address, acceptor.local_endpoint().port(), *token);
    if (!offer) {
        co_return common::Result<P2pHostUpgrade>::failure(offer.error());
    }
    if (!co_await write_exact(relay_stream, offer->data(), offer->size())) {
        co_return common::Result<P2pHostUpgrade>::failure(common::ErrorCode::connection_failed,
                                                          "P2P offer could not be sent");
    }
    auto race =
        std::make_shared<HostRace>(relay_stream, std::move(acceptor), *token, negotiation_timeout);
    co_return co_await race->run();
}

asio::awaitable<common::Result<P2pPeerUpgrade>>
connect_p2p_upgrade(asio::ip::tcp::socket bootstrap_socket,
                    const std::chrono::seconds negotiation_timeout,
                    const std::chrono::seconds direct_connect_timeout, const bool direct_enabled) {
    if (!bootstrap_socket.is_open() || !valid_timeout(negotiation_timeout) ||
        !valid_timeout(direct_connect_timeout) || direct_connect_timeout > negotiation_timeout) {
        co_return common::Result<P2pPeerUpgrade>::failure(common::ErrorCode::invalid_argument,
                                                          "P2P peer options are invalid");
    }
    auto bootstrap = std::make_shared<asio::ip::tcp::socket>(std::move(bootstrap_socket));
    auto direct = std::make_shared<asio::ip::tcp::socket>(bootstrap->get_executor());
    asio::steady_timer negotiation_timer{bootstrap->get_executor()};
    negotiation_timer.expires_after(negotiation_timeout);
    negotiation_timer.async_wait([bootstrap, direct](const asio::error_code& error) {
        if (!error) {
            close_socket(*bootstrap);
            close_socket(*direct);
        }
    });
    auto offer = co_await read_offer(*bootstrap);
    if (!offer) {
        static_cast<void>(negotiation_timer.cancel());
        co_return common::Result<P2pPeerUpgrade>::failure(offer.error());
    }
    if (direct_enabled) {
        asio::steady_timer direct_timer{bootstrap->get_executor()};
        direct_timer.expires_after(direct_connect_timeout);
        direct_timer.async_wait([direct](const asio::error_code& error) {
            if (!error) {
                close_socket(*direct);
            }
        });
        asio::error_code connect_error;
        co_await direct->async_connect(offer->candidate,
                                       asio::redirect_error(asio::use_awaitable, connect_error));
        static_cast<void>(direct_timer.cancel());
        if (!connect_error) {
            std::array<std::uint8_t, kDirectHandshakeSize> handshake{};
            std::copy(kDirectMagic.begin(), kDirectMagic.end(), handshake.begin());
            std::copy(offer->token.begin(), offer->token.end(),
                      handshake.begin() + static_cast<std::ptrdiff_t>(kDirectMagic.size()));
            if (co_await write_exact(*direct, handshake.data(), handshake.size())) {
                asio::ssl::context direct_tls_context{asio::ssl::context::tlsv13_client};
                auto direct_stream =
                    std::make_shared<TlsStream>(std::move(*direct), direct_tls_context);
                configure_direct_tls(*direct_stream, offer->token, false);
                asio::error_code handshake_error;
                co_await direct_stream->async_handshake(
                    asio::ssl::stream_base::client,
                    asio::redirect_error(asio::use_awaitable, handshake_error));
                if (!handshake_error) {
                    std::array<std::uint8_t, kReadyMagic.size()> ready{};
                    if (co_await read_exact(*direct_stream, ready.data(), ready.size()) &&
                        ready == kReadyMagic) {
                        static_cast<void>(negotiation_timer.cancel());
                        close_socket(*bootstrap);
                        co_return P2pPeerUpgrade{
                            P2pPath::direct,
                            nullptr,
                            std::make_unique<TlsStream>(std::move(*direct_stream))};
                    }
                }
                close_tls_stream(*direct_stream);
            }
        }
    }
    close_socket(*direct);
    if (!bootstrap->is_open() ||
        !co_await write_exact(*bootstrap, kFallbackMagic.data(), kFallbackMagic.size())) {
        static_cast<void>(negotiation_timer.cancel());
        co_return common::Result<P2pPeerUpgrade>::failure(
            common::ErrorCode::connection_failed, "P2P relay fallback could not be requested");
    }
    std::array<std::uint8_t, kReadyMagic.size()> ready{};
    if (!co_await read_exact(*bootstrap, ready.data(), ready.size()) || ready != kReadyMagic) {
        static_cast<void>(negotiation_timer.cancel());
        co_return common::Result<P2pPeerUpgrade>::failure(common::ErrorCode::connection_failed,
                                                          "P2P relay fallback was not confirmed");
    }
    static_cast<void>(negotiation_timer.cancel());
    co_return P2pPeerUpgrade{P2pPath::relay,
                             std::make_unique<asio::ip::tcp::socket>(std::move(*bootstrap)),
                             nullptr};
}

asio::awaitable<common::Result<void>> confirm_p2p_direct(TlsStream& stream) {
    if (!co_await write_exact(stream, kReadyMagic.data(), kReadyMagic.size())) {
        co_return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                "P2P direct path could not be confirmed");
    }
    co_return common::Result<void>::success();
}

asio::awaitable<common::Result<void>> confirm_p2p_relay(TlsStream& stream) {
    if (!co_await write_exact(stream, kReadyMagic.data(), kReadyMagic.size())) {
        co_return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                "P2P relay fallback could not be confirmed");
    }
    co_return common::Result<void>::success();
}

asio::awaitable<common::Result<P2pRelayStats>>
relay_tcp_and_tcp(asio::ip::tcp::socket& first, asio::ip::tcp::socket& second,
                  const std::chrono::seconds inactivity_timeout) {
    if (!first.is_open() || !second.is_open() || !valid_timeout(inactivity_timeout)) {
        co_return common::Result<P2pRelayStats>::failure(
            common::ErrorCode::invalid_argument, "P2P relay options or sockets are invalid");
    }
    auto operation = std::make_shared<TcpRelayOperation>(first, second, inactivity_timeout);
    co_return co_await operation->run();
}

} // namespace minitun::protocol
