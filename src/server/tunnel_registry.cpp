#include <minitun/server/tunnel_registry.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/logging.hpp>
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
    class Listener final : public std::enable_shared_from_this<Listener> {
      public:
        Listener(const asio::any_io_executor& listener_executor,
                 const asio::any_io_executor& connection_executor,
                 TunnelBinding binding, PublicConnectionHandler connection_handler,
                 std::shared_ptr<ReservedFileDescriptor> reserved_descriptor)
            : binding_(std::move(binding)), acceptor_(listener_executor),
              retry_timer_(acceptor_.get_executor()),
              connection_executor_(connection_executor),
              connection_handler_(std::move(connection_handler)),
              reserved_descriptor_(std::move(reserved_descriptor)) {}

        ~Listener() noexcept { stop(); }

        [[nodiscard]] common::Result<void> start() {
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

        void stop() noexcept {
            asio::error_code ignored;
            try {
                static_cast<void>(retry_timer_.cancel());
            } catch (...) {
            }
            acceptor_.cancel(ignored);
            acceptor_.close(ignored);
        }

        [[nodiscard]] const TunnelBinding& binding() const noexcept { return binding_; }

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

  public:
    Impl(asio::any_io_executor listener_executor, asio::any_io_executor connection_executor,
         common::PortRange allowed_ports, const std::size_t max_tunnels_per_client,
         const std::size_t max_total_tunnels, PublicConnectionHandler connection_handler)
        : listener_executor_(std::move(listener_executor)),
          connection_executor_(std::move(connection_executor)),
          allowed_ports_(allowed_ports), max_tunnels_per_client_(max_tunnels_per_client),
          max_total_tunnels_(max_total_tunnels), connection_handler_(std::move(connection_handler)),
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
        auto listener =
            std::make_shared<Listener>(listener_executor_, connection_executor_, binding,
                                       connection_handler_, reserved_descriptor_);
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
    std::shared_ptr<ReservedFileDescriptor> reserved_descriptor_;
    std::unordered_map<std::string, std::shared_ptr<Listener>> listeners_;
};

TunnelRegistry::TunnelRegistry(const asio::any_io_executor& executor,
                               common::PortRange allowed_ports,
                               const std::size_t max_tunnels_per_client,
                               PublicConnectionHandler connection_handler)
    : TunnelRegistry(executor, executor, allowed_ports, max_tunnels_per_client,
                     kMaximumTotalTunnels, std::move(connection_handler)) {}

TunnelRegistry::TunnelRegistry(const asio::any_io_executor& executor,
                               common::PortRange allowed_ports,
                               const std::size_t max_tunnels_per_client,
                               const std::size_t max_total_tunnels,
                               PublicConnectionHandler connection_handler)
    : TunnelRegistry(executor, executor, allowed_ports, max_tunnels_per_client,
                     max_total_tunnels, std::move(connection_handler)) {}

TunnelRegistry::TunnelRegistry(asio::any_io_executor listener_executor,
                               asio::any_io_executor connection_executor,
                               common::PortRange allowed_ports,
                               const std::size_t max_tunnels_per_client,
                               PublicConnectionHandler connection_handler)
    : TunnelRegistry(std::move(listener_executor), std::move(connection_executor),
                     allowed_ports, max_tunnels_per_client, kMaximumTotalTunnels,
                     std::move(connection_handler)) {}

TunnelRegistry::TunnelRegistry(asio::any_io_executor listener_executor,
                               asio::any_io_executor connection_executor,
                               common::PortRange allowed_ports,
                               const std::size_t max_tunnels_per_client,
                               const std::size_t max_total_tunnels,
                               PublicConnectionHandler connection_handler)
    : implementation_(std::make_unique<Impl>(
          std::move(listener_executor), std::move(connection_executor), allowed_ports,
          max_tunnels_per_client, max_total_tunnels, std::move(connection_handler))) {}

TunnelRegistry::~TunnelRegistry() noexcept = default;

common::Result<void> TunnelRegistry::register_tunnel(const TunnelBinding& binding,
                                                     const std::size_t max_for_client) {
    return implementation_->register_tunnel(binding, max_for_client);
}

void TunnelRegistry::unregister_tunnel(const std::string_view client_id,
                                       const std::uint64_t session_generation,
                                       const std::string_view tunnel_id,
                                       const std::uint64_t config_revision) noexcept {
    implementation_->unregister_tunnel(client_id, session_generation, tunnel_id,
                                       config_revision);
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
