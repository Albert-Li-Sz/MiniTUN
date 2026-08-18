#include <minitun/daemon/worker_pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/dispatch.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/time.hpp>
#include <minitun/protocol/auth.hpp>
#include <minitun/protocol/datagram.hpp>
#include <minitun/protocol/messages.hpp>
#include <minitun/protocol/p2p.hpp>
#include <minitun/protocol/relay.hpp>
#include <minitun/protocol/socks5.hpp>
#include <minitun/protocol/state_machine.hpp>
#include <minitun/protocol/tls.hpp>

namespace minitun::daemon {
namespace {

inline constexpr std::chrono::seconds kMaximumWorkerTimeout{300};
inline constexpr std::chrono::seconds kMaximumWorkerIdleTimeout{305};
inline constexpr std::chrono::hours kMaximumRelayTimeout{24};

[[nodiscard]] common::Result<void> validate_options(const WorkerPoolOptions& options) {
    if (!common::Id::parse(options.server_id, common::IdKind::server) ||
        !common::Id::parse(options.remote_server_id, common::IdKind::server) ||
        !common::Id::parse(options.client_id, common::IdKind::client) || options.psk == nullptr ||
        options.psk->empty() || options.session_generation == 0U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "worker pool identity is invalid");
    }
    if (options.min_idle_workers > options.max_idle_workers || options.max_idle_workers > 128U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "worker pool limits are invalid");
    }
    if (options.connect_timeout <= std::chrono::seconds::zero() ||
        options.connect_timeout > kMaximumWorkerTimeout ||
        options.handshake_timeout <= std::chrono::seconds::zero() ||
        options.handshake_timeout > kMaximumWorkerTimeout ||
        options.idle_timeout <= std::chrono::seconds::zero() ||
        options.idle_timeout > kMaximumWorkerIdleTimeout ||
        options.relay_inactivity_timeout <= std::chrono::seconds::zero() ||
        options.relay_inactivity_timeout > kMaximumRelayTimeout ||
        options.graceful_shutdown_timeout <= std::chrono::seconds::zero() ||
        options.graceful_shutdown_timeout > kMaximumWorkerTimeout) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "worker pool timeout is invalid");
    }
    return common::Result<void>::success();
}

} // namespace

WorkerBudget::WorkerBudget(const std::size_t maximum) noexcept
    : maximum_(std::max<std::size_t>(maximum, 1U)) {}

bool WorkerBudget::try_acquire() noexcept {
    std::size_t current = in_use_.load();
    while (current < maximum_) {
        if (in_use_.compare_exchange_weak(current, current + 1U)) {
            return true;
        }
    }
    return false;
}

void WorkerBudget::release() noexcept {
    std::size_t current = in_use_.load();
    while (current > 0U && !in_use_.compare_exchange_weak(current, current - 1U)) {
    }
}

std::size_t WorkerBudget::in_use() const noexcept { return in_use_.load(); }

std::size_t WorkerBudget::maximum() const noexcept { return maximum_; }

class WorkerPool::Impl final : public std::enable_shared_from_this<WorkerPool::Impl> {
  private:
    class WorkerSession final : public std::enable_shared_from_this<WorkerSession> {
      public:
        WorkerSession(const std::shared_ptr<Impl>& pool, std::string worker_id,
                      std::shared_ptr<WorkerBudget> connection_budget)
            : pool_(pool), worker_id_(std::move(worker_id)), server_id_(pool->options_.server_id),
              remote_endpoint_(pool->options_.endpoint.to_string()), resolver_(pool->executor_),
              udp_resolver_(pool->executor_), stream_(pool->executor_, *pool->tls_context_),
              operation_timer_(pool->executor_),
              state_(protocol::PeerRole::client, protocol::ConnectionKind::worker),
              connection_budget_(std::move(connection_budget)) {}

        ~WorkerSession() noexcept {
            close();
            connection_budget_->release();
        }

        void start() {
            auto self = shared_from_this();
            asio::co_spawn(
                stream_.get_executor(), run(), [self](const std::exception_ptr& failure) {
                    if (failure) {
                        common::log_warn("worker session ended with an exception",
                                         self->log_context(common::ErrorCode::internal_error));
                    }
                    self->close();
                    if (auto pool = self->pool_.lock()) {
                        pool->worker_finished(self->worker_id_);
                    }
                });
        }

        void stop() noexcept { close(); }

      private:
#if defined(__GNUC__) && !defined(__clang__)
        // GCC 13 reports mismatched-new-delete on coroutine frames for this
        // long-running worker coroutine even though the frame is allocated and
        // released by the Asio awaitable machinery with matching sizes.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
        [[nodiscard]] asio::awaitable<void> run() {
            auto pool = pool_.lock();
            if (!pool || !pool->running_.load()) {
                co_return;
            }

            arm_timeout(pool->options_.connect_timeout);
            asio::error_code error;
            auto endpoints = co_await resolver_.async_resolve(
                pool->options_.endpoint.host(), std::to_string(pool->options_.endpoint.port()),
                asio::redirect_error(asio::use_awaitable, error));
            if (!error) {
                co_await asio::async_connect(stream_.lowest_layer(), endpoints,
                                             asio::redirect_error(asio::use_awaitable, error));
            }
            cancel_timeout();
            if (error) {
                co_return;
            }
            protocol::configure_tcp_transport(stream_.lowest_layer());

            auto configured = protocol::configure_client_tls_stream(
                stream_,
                pool->options_.tls_server_name.empty() ? pool->options_.endpoint.host()
                                                       : pool->options_.tls_server_name,
                pool->options_.insecure_skip_verify);
            if (!configured) {
                co_return;
            }
            if (pool->options_.tls_session_cache) {
                static_cast<void>(pool->options_.tls_session_cache->restore(stream_));
            }
            arm_timeout(pool->options_.handshake_timeout);
            co_await stream_.async_handshake(asio::ssl::stream_base::client,
                                             asio::redirect_error(asio::use_awaitable, error));
            cancel_timeout();
            if (error) {
                co_return;
            }
            if (protocol::tls_session_reused(stream_) && pool->options_.tls_resumption_handler) {
                try {
                    pool->options_.tls_resumption_handler();
                } catch (...) {
                }
            }

            auto nonce = protocol::generate_authentication_nonce();
            if (!nonce) {
                co_return;
            }
            const std::int64_t timestamp = common::unix_seconds_now();
            auto authentication_data = protocol::compute_worker_authentication_data(
                pool->options_.psk->view(), pool->options_.client_id,
                pool->options_.remote_server_id, pool->options_.session_generation, worker_id_,
                timestamp, *nonce);
            if (!authentication_data) {
                co_return;
            }
            auto hello_payload = protocol::encode_worker_hello({
                .client_id = pool->options_.client_id,
                .session_generation = pool->options_.session_generation,
                .worker_id = worker_id_,
                .timestamp_seconds = timestamp,
                .nonce = *nonce,
                .authentication_data = *authentication_data,
            });
            if (!hello_payload) {
                co_return;
            }
            const protocol::Frame hello_frame{protocol::MessageType::worker_hello, 0U, 1U,
                                              std::move(*hello_payload)};
            if (!co_await write_frame(hello_frame, pool->options_.handshake_timeout)) {
                co_return;
            }

            auto accepted_frame = co_await read_frame(pool->options_.handshake_timeout);
            if (!accepted_frame || accepted_frame->type != protocol::MessageType::worker_accepted) {
                co_return;
            }
            auto accepted = protocol::decode_worker_accepted(accepted_frame->payload);
            if (!accepted || accepted->worker_id != worker_id_) {
                co_return;
            }
            if (pool->options_.tls_session_cache) {
                static_cast<void>(pool->options_.tls_session_cache->capture(stream_));
            }

            auto relay_frame = co_await read_frame(pool->idle_timeout());
            if (!relay_frame || relay_frame->type != protocol::MessageType::start_relay) {
                co_return;
            }
            auto relay = protocol::decode_start_relay(relay_frame->payload);
            if (!relay) {
                co_return;
            }
            pool->worker_consumed(worker_id_);

            if (relay->mode == protocol::TunnelMode::udp) {
                auto local_endpoint = pool->local_endpoint_resolver_(relay->tunnel_id);
                if (!local_endpoint) {
                    co_await send_local_error(relay->connection_id, relay_frame->request_id,
                                              pool->options_.handshake_timeout);
                    co_return;
                }
                arm_timeout(pool->options_.connect_timeout);
                auto local_endpoints = co_await udp_resolver_.async_resolve(
                    local_endpoint->host(), std::to_string(local_endpoint->port()),
                    asio::redirect_error(asio::use_awaitable, error));
                if (!error && local_endpoints.begin() != local_endpoints.end()) {
                    local_udp_socket_ = std::make_unique<asio::ip::udp::socket>(
                        stream_.get_executor(), local_endpoints.begin()->endpoint().protocol());
                    co_await local_udp_socket_->async_connect(
                        local_endpoints.begin()->endpoint(),
                        asio::redirect_error(asio::use_awaitable, error));
                }
                cancel_timeout();
                if (error || local_udp_socket_ == nullptr) {
                    co_await send_local_error(relay->connection_id, relay_frame->request_id,
                                              pool->options_.handshake_timeout);
                    co_return;
                }
                if (!co_await send_local_connected(relay->connection_id, relay_frame->request_id,
                                                   pool->options_.handshake_timeout)) {
                    co_return;
                }
                auto relayed = co_await protocol::relay_tls_and_udp(
                    stream_, *local_udp_socket_,
                    {.inactivity_timeout = pool->options_.relay_inactivity_timeout});
                if (relayed && pool->options_.relay_stats_handler) {
                    try {
                        pool->options_.relay_stats_handler(relayed->tls_to_udp_bytes,
                                                           relayed->udp_to_tls_bytes);
                    } catch (...) {
                    }
                }
                if (!relayed && relayed.error().code() != common::ErrorCode::connection_timeout) {
                    common::log_warn("UDP Worker relay ended with a transport error",
                                     log_context(relayed.error().code()));
                }
                co_return;
            }

            if (relay->mode == protocol::TunnelMode::socks5) {
                local_socket_ = std::make_unique<asio::ip::tcp::socket>(stream_.get_executor());
                if (!co_await send_local_connected(relay->connection_id, relay_frame->request_id,
                                                   pool->options_.handshake_timeout)) {
                    co_return;
                }
                arm_timeout(pool->options_.connect_timeout);
                auto socks_result =
                    co_await protocol::accept_socks5_connect(stream_, *local_socket_, resolver_);
                cancel_timeout();
                if (!socks_result) {
                    if (socks_result.error().code() != common::ErrorCode::connection_failed) {
                        common::log_warn("SOCKS5 Worker request was rejected",
                                         log_context(socks_result.error().code()));
                    }
                    co_return;
                }
                protocol::configure_tcp_transport(*local_socket_);
                co_await relay_connected_socket(*pool);
                co_return;
            }

            if (relay->mode == protocol::TunnelMode::p2p) {
                auto local_endpoint = pool->local_endpoint_resolver_(relay->tunnel_id);
                asio::error_code endpoint_error;
                const auto candidate_address =
                    stream_.lowest_layer().local_endpoint(endpoint_error).address();
                if (!local_endpoint || endpoint_error || candidate_address.is_unspecified()) {
                    co_await send_local_error(relay->connection_id, relay_frame->request_id,
                                              pool->options_.handshake_timeout);
                    co_return;
                }
                if (!co_await send_local_connected(relay->connection_id, relay_frame->request_id,
                                                   pool->options_.handshake_timeout)) {
                    co_return;
                }
                std::optional<asio::ip::tcp::endpoint> peer_observed_endpoint;
                if (relay->source_host.has_value() && relay->source_port.has_value()) {
                    asio::error_code peer_address_error;
                    const auto peer_address =
                        asio::ip::make_address(*relay->source_host, peer_address_error);
                    if (!peer_address_error && !peer_address.is_unspecified()) {
                        peer_observed_endpoint =
                            asio::ip::tcp::endpoint{peer_address, *relay->source_port};
                    }
                }
                std::optional<asio::ip::address> advertised_address;
                if (relay->worker_observed_host.has_value()) {
                    asio::error_code observed_address_error;
                    const auto observed_address = asio::ip::make_address(
                        *relay->worker_observed_host, observed_address_error);
                    if (!observed_address_error && !observed_address.is_unspecified()) {
                        advertised_address = observed_address;
                    }
                }
                auto upgraded = co_await protocol::accept_p2p_upgrade(
                    stream_, candidate_address, std::move(advertised_address),
                    pool->options_.connect_timeout, std::move(peer_observed_endpoint),
                    pool->options_.simultaneous_open_enabled);
                if (upgraded && pool->options_.p2p_path_handler) {
                    try {
                        pool->options_.p2p_path_handler(upgraded->path == protocol::P2pPath::direct
                                                            ? "direct"
                                                            : "relay");
                    } catch (...) {
                    }
                }
                if (!upgraded) {
                    common::log_warn("P2P Worker negotiation failed",
                                     log_context(upgraded.error().code()));
                    co_return;
                }

                if (upgraded->transport == protocol::P2pTransport::udp) {
                    // UDP-over-P2P: the peer negotiated datagram records, so the
                    // local target is a UDP endpoint reached from a connected
                    // ephemeral socket over either P2P path.
                    local_udp_socket_ =
                        std::make_unique<asio::ip::udp::socket>(stream_.get_executor());
                    arm_timeout(pool->options_.connect_timeout);
                    auto udp_endpoints = co_await udp_resolver_.async_resolve(
                        local_endpoint->host(), std::to_string(local_endpoint->port()),
                        asio::redirect_error(asio::use_awaitable, error));
                    if (!error && udp_endpoints.empty()) {
                        error = asio::error::host_not_found;
                    }
                    if (!error) {
                        const auto& first = *udp_endpoints.begin();
                        local_udp_socket_->open(first.endpoint().protocol(), error);
                        if (!error) {
                            local_udp_socket_->bind(
                                asio::ip::udp::endpoint{first.endpoint().protocol(), 0U}, error);
                        }
                        if (!error) {
                            local_udp_socket_->connect(first.endpoint(), error);
                        }
                    }
                    cancel_timeout();
                    if (error) {
                        common::log_warn("P2P UDP local target is unreachable",
                                         log_context(common::ErrorCode::local_connect_failed));
                        co_return;
                    }
                    if (upgraded->path == protocol::P2pPath::direct &&
                        upgraded->direct_stream != nullptr) {
                        p2p_socket_ = std::move(upgraded->direct_stream);
                        auto confirmed = co_await protocol::confirm_p2p_direct(*p2p_socket_);
                        if (!confirmed) {
                            co_return;
                        }
                        auto relayed = co_await protocol::relay_tls_and_udp(
                            *p2p_socket_, *local_udp_socket_,
                            {.inactivity_timeout = pool->options_.relay_inactivity_timeout});
                        if (relayed && pool->options_.relay_stats_handler) {
                            try {
                                pool->options_.relay_stats_handler(relayed->tls_to_udp_bytes,
                                                                   relayed->udp_to_tls_bytes);
                            } catch (...) {
                            }
                        }
                        if (relayed && pool->options_.p2p_udp_stats_handler) {
                            try {
                                pool->options_.p2p_udp_stats_handler(
                                    "direct", relayed->tls_to_udp_datagrams,
                                    relayed->udp_to_tls_datagrams, relayed->tls_to_udp_bytes,
                                    relayed->udp_to_tls_bytes);
                            } catch (...) {
                            }
                        }
                        if (!relayed &&
                            relayed.error().code() != common::ErrorCode::connection_timeout) {
                            common::log_warn("direct P2P UDP relay ended with a transport error",
                                             log_context(relayed.error().code()));
                        }
                        co_return;
                    }
                    auto confirmed = co_await protocol::confirm_p2p_relay(stream_);
                    if (!confirmed) {
                        co_return;
                    }
                    auto relayed = co_await protocol::relay_tls_and_udp(
                        stream_, *local_udp_socket_,
                        {.inactivity_timeout = pool->options_.relay_inactivity_timeout});
                    if (relayed && pool->options_.relay_stats_handler) {
                        try {
                            pool->options_.relay_stats_handler(relayed->tls_to_udp_bytes,
                                                               relayed->udp_to_tls_bytes);
                        } catch (...) {
                        }
                    }
                    if (relayed && pool->options_.p2p_udp_stats_handler) {
                        try {
                            pool->options_.p2p_udp_stats_handler(
                                "relay", relayed->tls_to_udp_datagrams,
                                relayed->udp_to_tls_datagrams, relayed->tls_to_udp_bytes,
                                relayed->udp_to_tls_bytes);
                        } catch (...) {
                        }
                    }
                    if (!relayed &&
                        relayed.error().code() != common::ErrorCode::connection_timeout) {
                        common::log_warn("relay P2P UDP session ended with a transport error",
                                         log_context(relayed.error().code()));
                    }
                    co_return;
                }

                local_socket_ = std::make_unique<asio::ip::tcp::socket>(stream_.get_executor());
                arm_timeout(pool->options_.connect_timeout);
                auto local_endpoints = co_await resolver_.async_resolve(
                    local_endpoint->host(), std::to_string(local_endpoint->port()),
                    asio::redirect_error(asio::use_awaitable, error));
                if (!error) {
                    co_await asio::async_connect(*local_socket_, local_endpoints,
                                                 asio::redirect_error(asio::use_awaitable, error));
                }
                cancel_timeout();
                if (error) {
                    co_return;
                }
                protocol::configure_tcp_transport(*local_socket_);

                if (upgraded->path == protocol::P2pPath::direct &&
                    upgraded->direct_stream != nullptr) {
                    p2p_socket_ = std::move(upgraded->direct_stream);
                    auto confirmed = co_await protocol::confirm_p2p_direct(*p2p_socket_);
                    if (!confirmed) {
                        co_return;
                    }
                    auto relayed = co_await protocol::relay_tls_and_tcp(
                        *p2p_socket_, *local_socket_,
                        {.inactivity_timeout = pool->options_.relay_inactivity_timeout});
                    if (relayed && pool->options_.relay_stats_handler) {
                        try {
                            pool->options_.relay_stats_handler(relayed->tls_to_tcp_bytes,
                                                               relayed->tcp_to_tls_bytes);
                        } catch (...) {
                        }
                    }
                    if (!relayed &&
                        relayed.error().code() != common::ErrorCode::connection_timeout) {
                        common::log_warn("direct P2P relay ended with a transport error",
                                         log_context(relayed.error().code()));
                    }
                    co_return;
                }

                auto confirmed = co_await protocol::confirm_p2p_relay(stream_);
                if (!confirmed) {
                    co_return;
                }
                co_await relay_connected_socket(*pool);
                co_return;
            }

            auto local_endpoint = pool->local_endpoint_resolver_(relay->tunnel_id);
            if (!local_endpoint) {
                co_await send_local_error(relay->connection_id, relay_frame->request_id,
                                          pool->options_.handshake_timeout);
                co_return;
            }

            local_socket_ = std::make_unique<asio::ip::tcp::socket>(stream_.get_executor());
            arm_timeout(pool->options_.connect_timeout);
            auto local_endpoints = co_await resolver_.async_resolve(
                local_endpoint->host(), std::to_string(local_endpoint->port()),
                asio::redirect_error(asio::use_awaitable, error));
            if (!error) {
                co_await asio::async_connect(*local_socket_, local_endpoints,
                                             asio::redirect_error(asio::use_awaitable, error));
            }
            cancel_timeout();
            if (error) {
                co_await send_local_error(relay->connection_id, relay_frame->request_id,
                                          pool->options_.handshake_timeout);
                co_return;
            }
            protocol::configure_tcp_transport(*local_socket_);

            if (!co_await send_local_connected(relay->connection_id, relay_frame->request_id,
                                               pool->options_.handshake_timeout)) {
                co_return;
            }
            if (!co_await write_proxy_header(*relay, *pool)) {
                co_return;
            }

            co_await relay_connected_socket(*pool);
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

        [[nodiscard]] asio::awaitable<bool>
        send_local_connected(const std::string& connection_id, const std::uint64_t request_id,
                             const std::chrono::seconds timeout) {
            auto connected_payload = protocol::encode_local_connect_ok({connection_id});
            if (!connected_payload) {
                co_return false;
            }
            const protocol::Frame connected_frame{protocol::MessageType::local_connect_ok, 0U,
                                                  request_id, std::move(*connected_payload)};
            co_return co_await write_frame(connected_frame, timeout);
        }

        [[nodiscard]] asio::awaitable<bool> write_proxy_header(const protocol::StartRelayMessage& relay,
                                                              Impl& pool) {
            if (!pool.options_.proxy_protocol_resolver ||
                !pool.options_.proxy_protocol_resolver(relay.tunnel_id) ||
                !relay.source_host.has_value()) {
                co_return true;
            }
            asio::error_code endpoint_error;
            const auto destination = local_socket_->remote_endpoint(endpoint_error);
            const bool tcp6 = relay.source_host->find(':') != std::string::npos;
            std::string header = "PROXY ";
            header += tcp6 ? "TCP6 " : "TCP4 ";
            header += *relay.source_host;
            header += ' ';
            header += endpoint_error ? (tcp6 ? "::" : "0.0.0.0") : destination.address().to_string();
            header += ' ';
            header += std::to_string(relay.source_port.value_or(0U));
            header += ' ';
            header += endpoint_error ? "0" : std::to_string(destination.port());
            header += "\r\n";
            asio::error_code write_error;
            static_cast<void>(co_await asio::async_write(
                *local_socket_, asio::buffer(header),
                asio::redirect_error(asio::use_awaitable, write_error)));
            if (write_error) {
                auto context = log_context(common::ErrorCode::connection_failed);
                context.tunnel_id = relay.tunnel_id;
                common::log_warn("PROXY protocol header write failed", context);
                asio::error_code close_error;
                local_socket_->close(close_error);
                co_return false;
            }
            co_return true;
        }

        [[nodiscard]] asio::awaitable<void> relay_connected_socket(Impl& pool) {
            auto relayed = co_await protocol::relay_tls_and_tcp(
                stream_, *local_socket_,
                {.inactivity_timeout = pool.options_.relay_inactivity_timeout});
            if (relayed && pool.options_.relay_stats_handler) {
                try {
                    pool.options_.relay_stats_handler(relayed->tls_to_tcp_bytes,
                                                      relayed->tcp_to_tls_bytes);
                } catch (...) {
                }
            }
            if (!relayed && relayed.error().code() != common::ErrorCode::connection_timeout) {
                common::log_warn("worker relay ended with a transport error",
                                 log_context(relayed.error().code()));
            }
        }

        [[nodiscard]] asio::awaitable<void> send_local_error(const std::string& connection_id,
                                                             const std::uint64_t request_id,
                                                             const std::chrono::seconds timeout) {
            auto failed_payload = protocol::encode_local_connect_error(
                {connection_id, common::ErrorCode::local_connect_failed});
            if (failed_payload) {
                const protocol::Frame failed_frame{protocol::MessageType::local_connect_error, 0U,
                                                   request_id, std::move(*failed_payload)};
                static_cast<void>(co_await write_frame(failed_frame, timeout));
            }
        }

        [[nodiscard]] asio::awaitable<common::Result<protocol::Frame>>
        read_frame(const std::chrono::seconds timeout) {
            arm_timeout(timeout);
            auto frame = co_await protocol::async_read_frame(stream_);
            cancel_timeout();
            if (!frame) {
                co_return frame;
            }
            auto transition = state_.on_receive(frame->type);
            if (!transition) {
                co_return common::Result<protocol::Frame>::failure(transition.error());
            }
            co_return frame;
        }

        [[nodiscard]] asio::awaitable<bool> write_frame(const protocol::Frame& frame,
                                                        const std::chrono::seconds timeout) {
            auto transition = state_.on_send(frame.type);
            if (!transition) {
                co_return false;
            }
            arm_timeout(timeout);
            auto written = co_await protocol::async_write_frame(stream_, frame);
            cancel_timeout();
            co_return static_cast<bool>(written);
        }

        void arm_timeout(const std::chrono::seconds timeout) {
            operation_timer_.expires_after(timeout);
            auto weak = weak_from_this();
            operation_timer_.async_wait([weak](const asio::error_code& error) {
                if (!error) {
                    if (auto self = weak.lock()) {
                        self->resolver_.cancel();
                        self->udp_resolver_.cancel();
                        if (self->local_socket_ != nullptr) {
                            asio::error_code ignored;
                            self->local_socket_->cancel(ignored);
                            self->local_socket_->close(ignored);
                        }
                        if (self->local_udp_socket_ != nullptr) {
                            asio::error_code ignored;
                            self->local_udp_socket_->cancel(ignored);
                            self->local_udp_socket_->close(ignored);
                        }
                        if (self->p2p_socket_ != nullptr) {
                            protocol::close_tls_stream(*self->p2p_socket_);
                        }
                        protocol::close_tls_stream(self->stream_);
                    }
                }
            });
        }

        void cancel_timeout() noexcept {
            try {
                static_cast<void>(operation_timer_.cancel());
            } catch (...) {
            }
        }

        void close() noexcept {
            resolver_.cancel();
            udp_resolver_.cancel();
            cancel_timeout();
            if (local_socket_ != nullptr) {
                asio::error_code ignored;
                local_socket_->cancel(ignored);
                local_socket_->close(ignored);
                local_socket_.reset();
            }
            if (local_udp_socket_ != nullptr) {
                asio::error_code ignored;
                local_udp_socket_->cancel(ignored);
                local_udp_socket_->close(ignored);
                local_udp_socket_.reset();
            }
            if (p2p_socket_ != nullptr) {
                protocol::close_tls_stream(*p2p_socket_);
                p2p_socket_.reset();
            }
            protocol::close_tls_stream(stream_);
        }

        [[nodiscard]] common::LogContext
        log_context(const std::optional<common::ErrorCode> error = std::nullopt) const noexcept {
            return {
                .component = "daemon.worker",
                .server_id = server_id_,
                .connection_id = worker_id_,
                .remote_endpoint = remote_endpoint_,
                .error_code = error,
            };
        }

        std::weak_ptr<Impl> pool_;
        std::string worker_id_;
        std::string server_id_;
        std::string remote_endpoint_;
        asio::ip::tcp::resolver resolver_;
        asio::ip::udp::resolver udp_resolver_;
        protocol::TlsStream stream_;
        std::unique_ptr<asio::ip::tcp::socket> local_socket_;
        std::unique_ptr<asio::ip::udp::socket> local_udp_socket_;
        std::unique_ptr<protocol::TlsStream> p2p_socket_;
        asio::steady_timer operation_timer_;
        protocol::StateMachine state_;
        std::shared_ptr<WorkerBudget> connection_budget_;
    };

  public:
    [[nodiscard]] static common::Result<std::shared_ptr<Impl>>
    create(asio::any_io_executor executor, std::shared_ptr<asio::ssl::context> tls_context,
           std::shared_ptr<WorkerBudget> idle_budget,
           std::shared_ptr<WorkerBudget> connection_budget, WorkerPoolOptions options,
           LocalEndpointResolver local_endpoint_resolver) {
        auto valid = validate_options(options);
        if (!valid) {
            return common::Result<std::shared_ptr<Impl>>::failure(valid.error());
        }
        if (!tls_context || !idle_budget || !connection_budget || !local_endpoint_resolver) {
            return common::Result<std::shared_ptr<Impl>>::failure(
                common::ErrorCode::invalid_argument, "worker pool dependency is unavailable");
        }
        return std::shared_ptr<Impl>{new Impl(
            std::move(executor), std::move(tls_context), std::move(idle_budget),
            std::move(connection_budget), std::move(options), std::move(local_endpoint_resolver))};
    }

    ~Impl() noexcept { stop(); }

    [[nodiscard]] common::Result<void> start() {
        if (running_.exchange(true)) {
            return common::Result<void>::failure(common::ErrorCode::already_exists,
                                                 "worker pool is already running");
        }
        auto self = shared_from_this();
        asio::dispatch(executor_, [self] { self->ensure_minimum(); });
        return common::Result<void>::success();
    }

    void request_workers(const std::uint16_t count) {
        if (count == 0U) {
            return;
        }
        auto self = shared_from_this();
        asio::dispatch(executor_, [self, count] {
            if (self->running_.load()) {
                self->spawn_workers(count);
            }
        });
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }
        auto self = shared_from_this();
        asio::dispatch(executor_, [self] {
            try {
                static_cast<void>(self->replenish_timer_.cancel());
            } catch (...) {
            }
            std::unordered_map<std::string, std::shared_ptr<WorkerSession>> idle_sessions;
            for (const auto& worker_id : self->available_workers_) {
                const auto iterator = self->sessions_.find(worker_id);
                if (iterator != self->sessions_.end()) {
                    idle_sessions.emplace(iterator->first, std::move(iterator->second));
                    self->sessions_.erase(iterator);
                }
            }
            const std::size_t budget_slots = self->available_workers_.size();
            self->available_workers_.clear();
            self->size_.store(0U);
            for (auto& [worker_id, session] : idle_sessions) {
                static_cast<void>(worker_id);
                session->stop();
            }
            for (std::size_t index = 0U; index < budget_slots; ++index) {
                self->budget_->release();
            }
            if (!self->sessions_.empty()) {
                self->shutdown_timer_.expires_after(self->options_.graceful_shutdown_timeout);
                const auto& shutdown_self = self;
                self->shutdown_timer_.async_wait([shutdown_self](const asio::error_code& error) {
                    if (!error) {
                        shutdown_self->force_stop();
                    }
                });
            }
        });
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_.load(); }

    [[nodiscard]] common::Result<void> set_idle_timeout(const std::chrono::seconds timeout) {
        if (timeout <= std::chrono::seconds::zero() || timeout > kMaximumWorkerIdleTimeout) {
            return common::Error{common::ErrorCode::invalid_argument,
                                 "Worker idle timeout is outside its limit"};
        }
        idle_timeout_seconds_.store(static_cast<std::uint32_t>(timeout.count()));
        return common::Result<void>::success();
    }

  private:
    Impl(asio::any_io_executor executor, std::shared_ptr<asio::ssl::context> tls_context,
         std::shared_ptr<WorkerBudget> idle_budget, std::shared_ptr<WorkerBudget> connection_budget,
         WorkerPoolOptions options, LocalEndpointResolver local_endpoint_resolver)
        : executor_(std::move(executor)), tls_context_(std::move(tls_context)),
          budget_(std::move(idle_budget)), connection_budget_(std::move(connection_budget)),
          options_(std::move(options)),
          local_endpoint_resolver_(std::move(local_endpoint_resolver)), replenish_timer_(executor_),
          shutdown_timer_(executor_),
          idle_timeout_seconds_(static_cast<std::uint32_t>(options_.idle_timeout.count())) {}

    [[nodiscard]] std::chrono::seconds idle_timeout() const noexcept {
        return std::chrono::seconds{idle_timeout_seconds_.load()};
    }

    void ensure_minimum() {
        const std::size_t current = available_workers_.size();
        const std::size_t minimum = options_.min_idle_workers;
        if (current < minimum) {
            spawn_workers(minimum - current);
        }
    }

    void spawn_workers(const std::size_t requested) {
        if (!running_.load() || requested == 0U) {
            return;
        }
        const std::size_t available =
            options_.max_idle_workers > available_workers_.size()
                ? static_cast<std::size_t>(options_.max_idle_workers) - available_workers_.size()
                : 0U;
        const std::size_t count = std::min(requested, available);
        for (std::size_t index = 0U; index < count; ++index) {
            if (!connection_budget_->try_acquire()) {
                notify_quota_rejection();
                break;
            }
            if (!budget_->try_acquire()) {
                connection_budget_->release();
                notify_quota_rejection();
                break;
            }
            auto worker_id = common::Id::generate(common::IdKind::connection);
            if (!worker_id) {
                budget_->release();
                connection_budget_->release();
                break;
            }
            const std::string key = worker_id->str();
            std::shared_ptr<WorkerSession> session;
            try {
                session =
                    std::make_shared<WorkerSession>(shared_from_this(), key, connection_budget_);
            } catch (...) {
                budget_->release();
                connection_budget_->release();
                break;
            }
            sessions_.emplace(key, session);
            available_workers_.insert(key);
            size_.store(available_workers_.size());
            session->start();
        }
    }

    void notify_quota_rejection() noexcept {
        if (!options_.quota_rejection_handler) {
            return;
        }
        try {
            options_.quota_rejection_handler();
        } catch (...) {
        }
    }

    void worker_consumed(const std::string_view worker_id) {
        const auto removed = available_workers_.erase(std::string{worker_id});
        if (removed == 0U) {
            return;
        }
        budget_->release();
        size_.store(available_workers_.size());
        ensure_minimum();
    }

    void worker_finished(const std::string_view worker_id) {
        const std::string key{worker_id};
        const auto removed = sessions_.erase(key);
        if (removed == 0U) {
            return;
        }
        if (available_workers_.erase(key) > 0U) {
            budget_->release();
        }
        size_.store(available_workers_.size());
        if (!running_.load()) {
            if (sessions_.empty()) {
                try {
                    static_cast<void>(shutdown_timer_.cancel());
                } catch (...) {
                }
            }
            return;
        }
        if (available_workers_.size() >= options_.min_idle_workers) {
            return;
        }
        replenish_timer_.expires_after(std::chrono::seconds{1});
        auto weak = weak_from_this();
        replenish_timer_.async_wait([weak](const asio::error_code& error) {
            if (!error) {
                if (auto self = weak.lock()) {
                    self->ensure_minimum();
                }
            }
        });
    }

    void force_stop() noexcept {
        auto sessions = std::move(sessions_);
        sessions_.clear();
        for (auto& [worker_id, session] : sessions) {
            static_cast<void>(worker_id);
            session->stop();
        }
    }

    asio::any_io_executor executor_;
    std::shared_ptr<asio::ssl::context> tls_context_;
    std::shared_ptr<WorkerBudget> budget_;
    std::shared_ptr<WorkerBudget> connection_budget_;
    WorkerPoolOptions options_;
    LocalEndpointResolver local_endpoint_resolver_;
    asio::steady_timer replenish_timer_;
    asio::steady_timer shutdown_timer_;
    std::unordered_map<std::string, std::shared_ptr<WorkerSession>> sessions_;
    std::unordered_set<std::string> available_workers_;
    std::atomic<std::size_t> size_{0U};
    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> idle_timeout_seconds_{305U};
};

common::Result<std::unique_ptr<WorkerPool>>
WorkerPool::create(asio::any_io_executor executor, std::shared_ptr<asio::ssl::context> tls_context,
                   std::shared_ptr<WorkerBudget> idle_budget,
                   std::shared_ptr<WorkerBudget> connection_budget, WorkerPoolOptions options,
                   LocalEndpointResolver local_endpoint_resolver) {
    auto implementation = Impl::create(std::move(executor), std::move(tls_context),
                                       std::move(idle_budget), std::move(connection_budget),
                                       std::move(options), std::move(local_endpoint_resolver));
    if (!implementation) {
        return common::Result<std::unique_ptr<WorkerPool>>::failure(implementation.error());
    }
    return std::unique_ptr<WorkerPool>{new WorkerPool{std::move(*implementation)}};
}

WorkerPool::WorkerPool(std::shared_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

WorkerPool::~WorkerPool() noexcept { stop(); }

common::Result<void> WorkerPool::start() { return implementation_->start(); }

common::Result<void> WorkerPool::set_idle_timeout(const std::chrono::seconds timeout) {
    return implementation_->set_idle_timeout(timeout);
}

void WorkerPool::request_workers(const std::uint16_t count) {
    implementation_->request_workers(count);
}

void WorkerPool::stop() noexcept { implementation_->stop(); }

std::size_t WorkerPool::size() const noexcept { return implementation_->size(); }

} // namespace minitun::daemon
