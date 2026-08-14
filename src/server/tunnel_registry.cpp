#include <minitun/server/tunnel_registry.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/write.hpp>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/protocol/datagram.hpp>
#include <minitun/protocol/tls.hpp>
#include <minitun/server/accept_recovery.hpp>

namespace minitun::server {
namespace {

inline constexpr std::size_t kMaximumTotalTunnels = 100'000U;

[[nodiscard]] std::string binding_key(const std::string_view client_id,
                                      const std::string_view tunnel_id) {
    std::string key;
    key.reserve(client_id.size() + tunnel_id.size() + 1U);
    key.append(client_id);
    key.push_back('/');
    key.append(tunnel_id);
    return key;
}

[[nodiscard]] common::Result<void> validate_binding(const TunnelBinding& binding,
                                                    const common::PortRange& allowed_ports) {
    if (!common::Id::parse(binding.client_id, common::IdKind::client) ||
        !common::Id::parse(binding.tunnel_id, common::IdKind::tunnel) ||
        binding.session_generation == 0U || binding.config_revision == 0U ||
        binding.bind_host.empty() || binding.bind_port == 0U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "tunnel binding is invalid");
    }
    if (!allowed_ports.contains(binding.bind_port)) {
        return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                             "remote tunnel port is outside the allowlist");
    }
    asio::error_code error;
    static_cast<void>(asio::ip::make_address(binding.bind_host, error));
    if (error) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "remote tunnel bind host must be numeric");
    }
    return common::Result<void>::success();
}

} // namespace

class TunnelRegistry::Impl final {
  private:
    class ListenerBase {
      public:
        virtual ~ListenerBase() noexcept = default;
        [[nodiscard]] virtual common::Result<void> start() = 0;
        virtual void stop() noexcept = 0;
        [[nodiscard]] virtual const TunnelBinding& binding() const noexcept = 0;
    };

    class TcpListener final : public ListenerBase,
                              public std::enable_shared_from_this<TcpListener> {
      public:
        TcpListener(const asio::any_io_executor& listener_executor,
                    const asio::any_io_executor& connection_executor, TunnelBinding binding,
                    PublicConnectionHandler connection_handler,
                    std::shared_ptr<ReservedFileDescriptor> reserved_descriptor)
            : binding_(std::move(binding)), acceptor_(listener_executor),
              retry_timer_(acceptor_.get_executor()), connection_executor_(connection_executor),
              connection_handler_(std::move(connection_handler)),
              reserved_descriptor_(std::move(reserved_descriptor)) {}

        ~TcpListener() noexcept override { stop(); }

        [[nodiscard]] common::Result<void> start() override {
            asio::error_code error;
            const auto address = asio::ip::make_address(binding_.bind_host, error);
            if (error) {
                return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                     "remote tunnel bind host is invalid");
            }
            const asio::ip::tcp::endpoint endpoint{address, binding_.bind_port};
            acceptor_.open(endpoint.protocol(), error);
            if (!error) {
                acceptor_.set_option(asio::socket_base::reuse_address{true}, error);
            }
            if (!error) {
                acceptor_.bind(endpoint, error);
            }
            if (!error) {
                acceptor_.listen(asio::socket_base::max_listen_connections, error);
            }
            if (error) {
                stop();
                common::ErrorCode code = common::ErrorCode::connection_failed;
                if (error == asio::error::address_in_use) {
                    code = common::ErrorCode::remote_port_in_use;
                } else if (error == asio::error::access_denied ||
                           error == asio::error::no_permission) {
                    code = common::ErrorCode::permission_denied;
                }
                return common::Result<void>::failure(code,
                                                     "remote tunnel listener could not be created");
            }
            accept_next();
            return common::Result<void>::success();
        }

        void stop() noexcept override {
            asio::error_code ignored;
            try {
                static_cast<void>(retry_timer_.cancel());
            } catch (...) {
            }
            acceptor_.cancel(ignored);
            acceptor_.close(ignored);
        }

        [[nodiscard]] const TunnelBinding& binding() const noexcept override { return binding_; }

      private:
        void accept_next() {
            if (!acceptor_.is_open()) {
                return;
            }
            auto self = shared_from_this();
            acceptor_.async_accept(
                asio::make_strand(connection_executor_),
                asio::bind_executor(
                    acceptor_.get_executor(),
                    [self](const asio::error_code& error, asio::ip::tcp::socket socket) mutable {
                        if (!error) {
                            protocol::configure_tcp_transport(socket);
                            self->retry_policy_.reset();
                            if (self->connection_handler_) {
                                try {
                                    self->connection_handler_(self->binding_, std::move(socket));
                                } catch (...) {
                                    asio::error_code ignored;
                                    socket.close(ignored);
                                }
                            } else {
                                asio::error_code ignored;
                                socket.close(ignored);
                            }
                        } else if (AcceptRetryPolicy::retryable(error)) {
                            self->handle_accept_failure(error);
                            return;
                        }
                        if (self->acceptor_.is_open()) {
                            self->accept_next();
                        }
                    }));
        }

        void handle_accept_failure(const asio::error_code& error) {
            if (AcceptRetryPolicy::descriptor_exhausted(error) && reserved_descriptor_) {
                reserved_descriptor_->recover(acceptor_);
            }
            if (retry_policy_.should_log(AcceptRetryPolicy::Clock::now())) {
                common::log_warn(
                    "public tunnel accept failed; retrying with backoff",
                    {.component = "server.tunnel-listener",
                     .tunnel_id = binding_.tunnel_id,
                     .error_code =
                         AcceptRetryPolicy::resource_exhausted(error)
                             ? std::optional<
                                   common::ErrorCode>{common::ErrorCode::resource_exhausted}
                             : std::optional<common::ErrorCode>{
                                   common::ErrorCode::connection_failed}});
            }
            retry_timer_.expires_after(retry_policy_.next_delay());
            auto self = shared_from_this();
            retry_timer_.async_wait([self](const asio::error_code& timer_error) {
                if (!timer_error && self->acceptor_.is_open()) {
                    self->accept_next();
                }
            });
        }

        TunnelBinding binding_;
        asio::ip::tcp::acceptor acceptor_;
        asio::steady_timer retry_timer_;
        asio::any_io_executor connection_executor_;
        PublicConnectionHandler connection_handler_;
        std::shared_ptr<ReservedFileDescriptor> reserved_descriptor_;
        AcceptRetryPolicy retry_policy_;
    };

    class UdpIngressBudget final {
      public:
        [[nodiscard]] bool reserve(const std::size_t bytes) noexcept {
            const std::scoped_lock lock{mutex_};
            if (queued_datagrams_ >= kMaximumDatagrams ||
                bytes > kMaximumBytes - std::min(queued_bytes_, kMaximumBytes)) {
                return false;
            }
            ++queued_datagrams_;
            queued_bytes_ += bytes;
            return true;
        }

        void release(const std::size_t datagrams, const std::size_t bytes) noexcept {
            const std::scoped_lock lock{mutex_};
            queued_datagrams_ -= std::min(queued_datagrams_, datagrams);
            queued_bytes_ -= std::min(queued_bytes_, bytes);
        }

      private:
        inline static constexpr std::size_t kMaximumDatagrams = 8'192U;
        inline static constexpr std::size_t kMaximumBytes = 64U * 1024U * 1024U;

        std::mutex mutex_;
        std::size_t queued_datagrams_{0U};
        std::size_t queued_bytes_{0U};
    };

    class UdpListener final : public ListenerBase,
                              public std::enable_shared_from_this<UdpListener> {
      private:
        inline static constexpr std::size_t kMaximumPeerSessions = 128U;
        inline static constexpr std::size_t kMaximumQueuedDatagrams = 1'024U;
        inline static constexpr std::size_t kMaximumQueuedBytes = 8U * 1024U * 1024U;
        inline static constexpr std::size_t kMaximumPeerQueuedDatagrams = 64U;
        inline static constexpr std::size_t kMaximumPeerQueuedBytes = 512U * 1024U;

        struct SocketPair final {
            std::shared_ptr<asio::ip::tcp::socket> bridge;
            asio::ip::tcp::socket relay;
        };

        class PeerSession final : public std::enable_shared_from_this<PeerSession> {
          public:
            PeerSession(std::weak_ptr<UdpListener> owner, std::string key,
                        asio::ip::udp::endpoint peer,
                        std::shared_ptr<asio::ip::tcp::socket> bridge)
                : owner_(std::move(owner)), key_(std::move(key)), peer_(std::move(peer)),
                  bridge_(std::move(bridge)) {}

            ~PeerSession() noexcept { close_socket(); }

            void start() {
                auto self = shared_from_this();
                asio::post(bridge_->get_executor(), [self] { self->read_header(); });
            }

            void enqueue(std::vector<std::uint8_t> record) {
                auto self = shared_from_this();
                asio::post(bridge_->get_executor(), [self, record = std::move(record)]() mutable {
                    if (self->stopped_ ||
                        self->write_queue_.size() >= kMaximumPeerQueuedDatagrams ||
                        record.size() >
                            kMaximumPeerQueuedBytes -
                                std::min(self->queued_bytes_, kMaximumPeerQueuedBytes)) {
                        self->release_ingress(1U, record.size());
                        return;
                    }
                    self->queued_bytes_ += record.size();
                    self->write_queue_.push_back(std::move(record));
                    if (!self->writing_) {
                        self->write_next();
                    }
                });
            }

            void stop() noexcept {
                auto self = shared_from_this();
                asio::post(bridge_->get_executor(), [self] { self->close(); });
            }

          private:
            void write_next() {
                if (stopped_ || write_queue_.empty()) {
                    writing_ = false;
                    return;
                }
                writing_ = true;
                auto self = shared_from_this();
                asio::async_write(*bridge_, asio::buffer(write_queue_.front()),
                                  [self](const asio::error_code& error, const std::size_t bytes) {
                                      if (error || self->write_queue_.empty() ||
                                          bytes != self->write_queue_.front().size()) {
                                          self->close();
                                          return;
                                      }
                                      const std::size_t record_size =
                                          self->write_queue_.front().size();
                                      self->queued_bytes_ -= record_size;
                                      self->write_queue_.pop_front();
                                      self->release_ingress(1U, record_size);
                                      self->write_next();
                                  });
            }

            void read_header() {
                if (stopped_) {
                    return;
                }
                auto self = shared_from_this();
                asio::async_read(*bridge_, asio::buffer(read_header_),
                                 [self](const asio::error_code& error, const std::size_t bytes) {
                                     if (error || bytes != self->read_header_.size()) {
                                         self->close();
                                         return;
                                     }
                                     const std::size_t length =
                                         (static_cast<std::size_t>(self->read_header_[0]) << 8U) |
                                         static_cast<std::size_t>(self->read_header_[1]);
                                     if (length > protocol::kMaximumUdpPayloadSize) {
                                         self->close();
                                         return;
                                     }
                                     self->read_payload_.assign(length, 0U);
                                     if (length == 0U) {
                                         self->deliver();
                                         return;
                                     }
                                     self->read_payload();
                                 });
            }

            void read_payload() {
                auto self = shared_from_this();
                asio::async_read(*bridge_, asio::buffer(read_payload_),
                                 [self](const asio::error_code& error, const std::size_t bytes) {
                                     if (error || bytes != self->read_payload_.size()) {
                                         self->close();
                                         return;
                                     }
                                     self->deliver();
                                 });
            }

            void deliver() {
                if (auto owner = owner_.lock()) {
                    owner->queue_send(peer_, std::move(read_payload_));
                    read_header();
                    return;
                }
                close();
            }

            void close() noexcept {
                if (stopped_) {
                    return;
                }
                stopped_ = true;
                close_socket();
                const std::size_t released_datagrams = write_queue_.size();
                const std::size_t released_bytes = queued_bytes_;
                write_queue_.clear();
                queued_bytes_ = 0U;
                release_ingress(released_datagrams, released_bytes);
                if (auto owner = owner_.lock()) {
                    owner->peer_finished(key_, this);
                }
            }

            void release_ingress(const std::size_t datagrams, const std::size_t bytes) noexcept {
                if (datagrams == 0U) {
                    return;
                }
                if (auto owner = owner_.lock()) {
                    owner->release_ingress(datagrams, bytes);
                }
            }

            void close_socket() noexcept {
                asio::error_code ignored;
                bridge_->cancel(ignored);
                bridge_->close(ignored);
            }

            std::weak_ptr<UdpListener> owner_;
            std::string key_;
            asio::ip::udp::endpoint peer_;
            std::shared_ptr<asio::ip::tcp::socket> bridge_;
            std::deque<std::vector<std::uint8_t>> write_queue_;
            std::array<std::uint8_t, protocol::kDatagramRecordHeaderSize> read_header_{};
            std::vector<std::uint8_t> read_payload_;
            std::size_t queued_bytes_{0U};
            bool writing_{false};
            bool stopped_{false};
        };

        struct PendingSend final {
            asio::ip::udp::endpoint peer;
            std::vector<std::uint8_t> payload;
        };

      public:
        UdpListener(const asio::any_io_executor& listener_executor,
                    const asio::any_io_executor& connection_executor, TunnelBinding binding,
                    PublicConnectionHandler connection_handler,
                    std::shared_ptr<UdpIngressBudget> global_ingress_budget,
                    const std::size_t max_peer_sessions)
            : binding_(std::move(binding)), socket_(listener_executor),
              retry_timer_(socket_.get_executor()), connection_executor_(connection_executor),
              connection_handler_(std::move(connection_handler)),
              global_ingress_budget_(std::move(global_ingress_budget)),
              max_peer_sessions_(max_peer_sessions == 0U ? kMaximumPeerSessions
                                                         : max_peer_sessions) {}

        ~UdpListener() noexcept override { stop(); }

        [[nodiscard]] common::Result<void> start() override {
            asio::error_code error;
            const auto address = asio::ip::make_address(binding_.bind_host, error);
            if (error) {
                return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                     "remote UDP bind host is invalid");
            }
            const asio::ip::udp::endpoint endpoint{address, binding_.bind_port};
            socket_.open(endpoint.protocol(), error);
            if (!error) {
                socket_.set_option(asio::socket_base::reuse_address{true}, error);
            }
            if (!error) {
                socket_.bind(endpoint, error);
            }
            if (error) {
                stop();
                const auto code =
                    error == asio::error::address_in_use ? common::ErrorCode::remote_port_in_use
                    : error == asio::error::access_denied || error == asio::error::no_permission
                        ? common::ErrorCode::permission_denied
                        : common::ErrorCode::connection_failed;
                return common::Result<void>::failure(code,
                                                     "remote UDP listener could not be created");
            }
            receive_next();
            return common::Result<void>::success();
        }

        void stop() noexcept override {
            asio::error_code ignored;
            try {
                static_cast<void>(retry_timer_.cancel());
            } catch (...) {
            }
            socket_.cancel(ignored);
            socket_.close(ignored);
            auto peers = std::move(peers_);
            peers_.clear();
            for (auto& [key, peer] : peers) {
                static_cast<void>(key);
                peer->stop();
            }
            release_ingress_now(queued_ingress_datagrams_, queued_ingress_bytes_);
            send_queue_.clear();
            queued_send_bytes_ = 0U;
            sending_ = false;
        }

        [[nodiscard]] const TunnelBinding& binding() const noexcept override { return binding_; }

      private:
        [[nodiscard]] static std::string peer_key(const asio::ip::udp::endpoint& peer) {
            return peer.address().to_string() + ":" + std::to_string(peer.port());
        }

        [[nodiscard]] common::Result<SocketPair> create_socket_pair() {
            asio::error_code error;
            auto peer_executor = asio::make_strand(connection_executor_);
            asio::ip::tcp::acceptor acceptor{peer_executor};
            const asio::ip::tcp::endpoint endpoint{asio::ip::address_v4::loopback(), 0U};
            acceptor.open(endpoint.protocol(), error);
            if (!error) {
                acceptor.bind(endpoint, error);
            }
            if (!error) {
                acceptor.listen(1, error);
            }
            asio::ip::tcp::socket relay{peer_executor};
            auto bridge = std::make_shared<asio::ip::tcp::socket>(peer_executor);
            if (!error) {
                bridge->connect(acceptor.local_endpoint(error), error);
            }
            if (!error) {
                acceptor.accept(relay, error);
            }
            if (error) {
                return common::Result<SocketPair>::failure(
                    common::ErrorCode::resource_exhausted,
                    "internal UDP relay socket pair could not be created");
            }
            protocol::configure_tcp_transport(*bridge);
            protocol::configure_tcp_transport(relay);
            return SocketPair{std::move(bridge), std::move(relay)};
        }

        void receive_next() {
            if (!socket_.is_open()) {
                return;
            }
            auto self = shared_from_this();
            socket_.async_receive_from(
                asio::buffer(receive_buffer_), receive_peer_,
                [self](const asio::error_code& error, const std::size_t bytes) {
                    if (!error && bytes <= protocol::kMaximumUdpPayloadSize) {
                        self->handle_datagram(self->receive_peer_,
                                              {self->receive_buffer_.data(), bytes});
                        self->receive_next();
                        return;
                    }
                    if (error == asio::error::operation_aborted || !self->socket_.is_open()) {
                        return;
                    }
                    self->retry_timer_.expires_after(std::chrono::milliseconds{100});
                    self->retry_timer_.async_wait([self](const asio::error_code& timer_error) {
                        if (!timer_error) {
                            self->receive_next();
                        }
                    });
                });
        }

        void handle_datagram(const asio::ip::udp::endpoint& peer,
                             const std::span<const std::uint8_t> payload) {
            auto record = protocol::encode_datagram_record(payload);
            if (!record || !reserve_ingress(record->size())) {
                return;
            }
            const std::string key = peer_key(peer);
            auto found = peers_.find(key);
            if (found == peers_.end()) {
                if (peers_.size() >= max_peer_sessions_) {
                    release_ingress_now(1U, record->size());
                    return;
                }
                auto sockets = create_socket_pair();
                if (!sockets) {
                    release_ingress_now(1U, record->size());
                    return;
                }
                auto session = std::make_shared<PeerSession>(weak_from_this(), key, peer,
                                                             std::move(sockets->bridge));
                found = peers_.emplace(key, session).first;
                session->start();
                if (connection_handler_) {
                    try {
                        connection_handler_(binding_, std::move(sockets->relay));
                    } catch (...) {
                        asio::error_code ignored;
                        sockets->relay.close(ignored);
                    }
                } else {
                    asio::error_code ignored;
                    sockets->relay.close(ignored);
                }
            }
            found->second->enqueue(std::move(*record));
        }

        [[nodiscard]] bool reserve_ingress(const std::size_t bytes) noexcept {
            if (queued_ingress_datagrams_ >= kMaximumQueuedDatagrams ||
                bytes >
                    kMaximumQueuedBytes - std::min(queued_ingress_bytes_, kMaximumQueuedBytes) ||
                !global_ingress_budget_->reserve(bytes)) {
                return false;
            }
            ++queued_ingress_datagrams_;
            queued_ingress_bytes_ += bytes;
            return true;
        }

        void release_ingress(const std::size_t datagrams, const std::size_t bytes) {
            auto self = shared_from_this();
            asio::post(socket_.get_executor(),
                       [self, datagrams, bytes] { self->release_ingress_now(datagrams, bytes); });
        }

        void release_ingress_now(const std::size_t datagrams, const std::size_t bytes) noexcept {
            const std::size_t released_datagrams = std::min(queued_ingress_datagrams_, datagrams);
            const std::size_t released_bytes = std::min(queued_ingress_bytes_, bytes);
            queued_ingress_datagrams_ -= released_datagrams;
            queued_ingress_bytes_ -= released_bytes;
            global_ingress_budget_->release(released_datagrams, released_bytes);
        }

        void peer_finished(std::string key, const PeerSession* const peer) {
            auto self = shared_from_this();
            asio::post(socket_.get_executor(), [self, key = std::move(key), peer] {
                const auto found = self->peers_.find(key);
                if (found != self->peers_.end() && found->second.get() == peer) {
                    self->peers_.erase(found);
                }
            });
        }

        void queue_send(asio::ip::udp::endpoint peer, std::vector<std::uint8_t> payload) {
            auto self = shared_from_this();
            asio::post(socket_.get_executor(), [self, peer = std::move(peer),
                                                payload = std::move(payload)]() mutable {
                if (!self->socket_.is_open() ||
                    self->send_queue_.size() >= kMaximumQueuedDatagrams ||
                    payload.size() > kMaximumQueuedBytes -
                                         std::min(self->queued_send_bytes_, kMaximumQueuedBytes)) {
                    return;
                }
                self->queued_send_bytes_ += payload.size();
                self->send_queue_.push_back({std::move(peer), std::move(payload)});
                if (!self->sending_) {
                    self->send_next();
                }
            });
        }

        void send_next() {
            if (!socket_.is_open() || send_queue_.empty()) {
                sending_ = false;
                return;
            }
            sending_ = true;
            auto self = shared_from_this();
            socket_.async_send_to(
                asio::buffer(send_queue_.front().payload), send_queue_.front().peer,
                [self](const asio::error_code&, const std::size_t) {
                    if (!self->send_queue_.empty()) {
                        self->queued_send_bytes_ -= self->send_queue_.front().payload.size();
                        self->send_queue_.pop_front();
                    }
                    self->send_next();
                });
        }

        TunnelBinding binding_;
        asio::ip::udp::socket socket_;
        asio::steady_timer retry_timer_;
        asio::any_io_executor connection_executor_;
        PublicConnectionHandler connection_handler_;
        std::shared_ptr<UdpIngressBudget> global_ingress_budget_;
        std::size_t max_peer_sessions_{kMaximumPeerSessions};
        std::array<std::uint8_t, protocol::kMaximumUdpPayloadSize> receive_buffer_{};
        asio::ip::udp::endpoint receive_peer_;
        std::unordered_map<std::string, std::shared_ptr<PeerSession>> peers_;
        std::deque<PendingSend> send_queue_;
        std::size_t queued_ingress_datagrams_{0U};
        std::size_t queued_ingress_bytes_{0U};
        std::size_t queued_send_bytes_{0U};
        bool sending_{false};
    };

  public:
    Impl(asio::any_io_executor listener_executor, asio::any_io_executor connection_executor,
         common::PortRange allowed_ports, const std::size_t max_tunnels_per_client,
         const std::size_t max_total_tunnels, PublicConnectionHandler connection_handler,
         const std::size_t max_udp_peer_sessions)
        : listener_executor_(std::move(listener_executor)),
          connection_executor_(std::move(connection_executor)), allowed_ports_(allowed_ports),
          max_tunnels_per_client_(max_tunnels_per_client), max_total_tunnels_(max_total_tunnels),
          connection_handler_(std::move(connection_handler)),
          udp_ingress_budget_(std::make_shared<UdpIngressBudget>()),
          max_udp_peer_sessions_(max_udp_peer_sessions),
          reserved_descriptor_(std::make_shared<ReservedFileDescriptor>()) {}

    [[nodiscard]] common::Result<void> register_tunnel(const TunnelBinding& binding,
                                                       const std::size_t max_for_client) {
        auto valid = validate_binding(binding, allowed_ports_);
        if (!valid) {
            return valid;
        }
        if (max_tunnels_per_client_ == 0U || max_tunnels_per_client_ > kMaximumTotalTunnels ||
            max_total_tunnels_ == 0U || max_total_tunnels_ > kMaximumTotalTunnels) {
            return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                 "tunnel limits are invalid");
        }
        const std::string key = binding_key(binding.client_id, binding.tunnel_id);
        const auto existing = listeners_.find(key);
        if (existing != listeners_.end()) {
            if (existing->second->binding() == binding) {
                return common::Result<void>::success();
            }
            existing->second->stop();
            listeners_.erase(existing);
        }
        const std::size_t effective_client_limit =
            max_for_client == 0U ? max_tunnels_per_client_
                                 : std::min(max_for_client, max_tunnels_per_client_);
        if (effective_client_limit == 0U ||
            client_size(binding.client_id) >= effective_client_limit) {
            return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                                 "client tunnel limit has been reached");
        }
        if (listeners_.size() >= max_total_tunnels_) {
            return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                                 "global tunnel limit has been reached");
        }
        std::shared_ptr<ListenerBase> listener;
        if (binding.mode == protocol::TunnelMode::udp) {
            listener =
                std::make_shared<UdpListener>(listener_executor_, connection_executor_, binding,
                                              connection_handler_, udp_ingress_budget_,
                                              max_udp_peer_sessions_);
        } else {
            listener =
                std::make_shared<TcpListener>(listener_executor_, connection_executor_, binding,
                                              connection_handler_, reserved_descriptor_);
        }
        auto started = listener->start();
        if (!started) {
            return started;
        }
        listeners_.emplace(key, std::move(listener));
        return common::Result<void>::success();
    }

    void unregister_tunnel(const std::string_view client_id, const std::uint64_t session_generation,
                           const std::string_view tunnel_id,
                           const std::uint64_t config_revision) noexcept {
        const auto iterator = listeners_.find(binding_key(client_id, tunnel_id));
        if (iterator == listeners_.end() ||
            iterator->second->binding().session_generation != session_generation ||
            (config_revision != 0U &&
             iterator->second->binding().config_revision != config_revision)) {
            return;
        }
        iterator->second->stop();
        listeners_.erase(iterator);
    }

    void remove_session(const std::string_view client_id,
                        const std::uint64_t session_generation) noexcept {
        erase_matching(client_id, session_generation, true);
    }

    void remove_client(const std::string_view client_id) noexcept {
        erase_matching(client_id, 0U, false);
    }

    void clear() noexcept {
        for (auto& [key, listener] : listeners_) {
            static_cast<void>(key);
            listener->stop();
        }
        listeners_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept { return listeners_.size(); }

    [[nodiscard]] std::size_t client_size(const std::string_view client_id) const noexcept {
        std::size_t count = 0U;
        for (const auto& [key, listener] : listeners_) {
            static_cast<void>(key);
            if (listener->binding().client_id == client_id) {
                ++count;
            }
        }
        return count;
    }

  private:
    void erase_matching(const std::string_view client_id, const std::uint64_t session_generation,
                        const bool match_generation) noexcept {
        for (auto iterator = listeners_.begin(); iterator != listeners_.end();) {
            const auto& binding = iterator->second->binding();
            if (binding.client_id == client_id &&
                (!match_generation || binding.session_generation == session_generation)) {
                iterator->second->stop();
                iterator = listeners_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    asio::any_io_executor listener_executor_;
    asio::any_io_executor connection_executor_;
    common::PortRange allowed_ports_;
    std::size_t max_tunnels_per_client_;
    std::size_t max_total_tunnels_;
    PublicConnectionHandler connection_handler_;
    std::shared_ptr<UdpIngressBudget> udp_ingress_budget_;
    std::size_t max_udp_peer_sessions_{128U};
    std::shared_ptr<ReservedFileDescriptor> reserved_descriptor_;
    std::unordered_map<std::string, std::shared_ptr<ListenerBase>> listeners_;
};

TunnelRegistry::TunnelRegistry(const asio::any_io_executor& executor,
                               common::PortRange allowed_ports,
                               const std::size_t max_tunnels_per_client,
                               PublicConnectionHandler connection_handler,
                               const std::size_t max_udp_peer_sessions)
    : TunnelRegistry(executor, executor, allowed_ports, max_tunnels_per_client,
                     kMaximumTotalTunnels, std::move(connection_handler),
                     max_udp_peer_sessions) {}

TunnelRegistry::TunnelRegistry(const asio::any_io_executor& executor,
                               common::PortRange allowed_ports,
                               const std::size_t max_tunnels_per_client,
                               const std::size_t max_total_tunnels,
                               PublicConnectionHandler connection_handler,
                               const std::size_t max_udp_peer_sessions)
    : TunnelRegistry(executor, executor, allowed_ports, max_tunnels_per_client, max_total_tunnels,
                     std::move(connection_handler), max_udp_peer_sessions) {}

TunnelRegistry::TunnelRegistry(asio::any_io_executor listener_executor,
                               asio::any_io_executor connection_executor,
                               common::PortRange allowed_ports,
                               const std::size_t max_tunnels_per_client,
                               PublicConnectionHandler connection_handler,
                               const std::size_t max_udp_peer_sessions)
    : TunnelRegistry(std::move(listener_executor), std::move(connection_executor), allowed_ports,
                     max_tunnels_per_client, kMaximumTotalTunnels, std::move(connection_handler),
                     max_udp_peer_sessions) {}

TunnelRegistry::TunnelRegistry(asio::any_io_executor listener_executor,
                               asio::any_io_executor connection_executor,
                               common::PortRange allowed_ports,
                               const std::size_t max_tunnels_per_client,
                               const std::size_t max_total_tunnels,
                               PublicConnectionHandler connection_handler,
                               const std::size_t max_udp_peer_sessions)
    : implementation_(std::make_unique<Impl>(
          std::move(listener_executor), std::move(connection_executor), allowed_ports,
          max_tunnels_per_client, max_total_tunnels, std::move(connection_handler),
          max_udp_peer_sessions)) {}

TunnelRegistry::~TunnelRegistry() noexcept = default;

common::Result<void> TunnelRegistry::register_tunnel(const TunnelBinding& binding,
                                                     const std::size_t max_for_client) {
    return implementation_->register_tunnel(binding, max_for_client);
}

void TunnelRegistry::unregister_tunnel(const std::string_view client_id,
                                       const std::uint64_t session_generation,
                                       const std::string_view tunnel_id,
                                       const std::uint64_t config_revision) noexcept {
    implementation_->unregister_tunnel(client_id, session_generation, tunnel_id, config_revision);
}

void TunnelRegistry::remove_session(const std::string_view client_id,
                                    const std::uint64_t session_generation) noexcept {
    implementation_->remove_session(client_id, session_generation);
}

void TunnelRegistry::remove_client(const std::string_view client_id) noexcept {
    implementation_->remove_client(client_id);
}

void TunnelRegistry::clear() noexcept { implementation_->clear(); }

std::size_t TunnelRegistry::size() const noexcept { return implementation_->size(); }

std::size_t TunnelRegistry::client_size(const std::string_view client_id) const noexcept {
    return implementation_->client_size(client_id);
}

} // namespace minitun::server
