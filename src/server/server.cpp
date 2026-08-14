#include <minitun/server/server.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>

#include <sys/resource.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/failpoint.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>
#include <minitun/protocol/auth.hpp>
#include <minitun/protocol/messages.hpp>
#include <minitun/protocol/relay.hpp>
#include <minitun/protocol/state_machine.hpp>
#include <minitun/protocol/tls.hpp>
#include <minitun/server/accept_recovery.hpp>
#include <minitun/server/client_policy.hpp>
#include <minitun/server/connection_quota.hpp>
#include <minitun/server/session_registry.hpp>
#include <minitun/server/tunnel_registry.hpp>
#include <minitun/server/worker_pool.hpp>

namespace minitun::server {
namespace {

inline constexpr std::size_t kMaxServerConnections = 100'000U;
inline constexpr std::size_t kMaxServerTunnels = 100'000U;
inline constexpr std::uint16_t kMaxWorkerRequestCount = 128U;
inline constexpr auto kWorkerRequestCooldown = std::chrono::milliseconds{100};
inline constexpr auto kWorkerRequestRetryAfter = std::chrono::seconds{1};
inline constexpr std::chrono::seconds kMaxConfiguredTimeout{300};
inline constexpr std::chrono::hours kMaximumRelayTimeout{24};

[[nodiscard]] common::Result<void> validate_options(const ServerOptions& options) {
    if (options.max_clients == 0U || options.max_clients > kMaxServerConnections) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "max clients is outside 1..100000");
    }
    if (options.max_tunnels_per_client == 0U || options.max_tunnels_per_client > 4'096U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "max tunnels per client is outside 1..4096");
    }
    if (options.max_total_tunnels == 0U || options.max_total_tunnels > kMaxServerTunnels) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "total tunnel limit is invalid");
    }
    if (options.max_connections_per_client == 0U ||
        options.max_connections_per_client > kMaxServerConnections ||
        options.max_total_connections == 0U ||
        options.max_total_connections > kMaxServerConnections ||
        options.max_connections_per_client > options.max_total_connections) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server connection limits are invalid");
    }
    if (options.handshake_timeout <= std::chrono::seconds::zero() ||
        options.handshake_timeout > kMaxConfiguredTimeout ||
        options.heartbeat_interval <= std::chrono::seconds::zero() ||
        options.heartbeat_interval > kMaxConfiguredTimeout ||
        options.heartbeat_timeout <= options.heartbeat_interval ||
        options.heartbeat_timeout > kMaxConfiguredTimeout ||
        options.allowed_clock_skew < std::chrono::seconds::zero() ||
        options.allowed_clock_skew > kMaxConfiguredTimeout ||
        options.worker_wait_timeout <= std::chrono::seconds::zero() ||
        options.worker_wait_timeout > kMaxConfiguredTimeout ||
        options.worker_idle_timeout <= std::chrono::seconds::zero() ||
        options.worker_idle_timeout > kMaxConfiguredTimeout ||
        options.relay_inactivity_timeout <= std::chrono::seconds::zero() ||
        options.relay_inactivity_timeout > kMaximumRelayTimeout ||
        options.graceful_shutdown_timeout <= std::chrono::seconds::zero() ||
        options.graceful_shutdown_timeout > kMaxConfiguredTimeout) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server timeout configuration is invalid");
    }
    if (options.min_idle_workers > options.max_idle_workers || options.max_idle_workers > 128U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server worker limits are invalid");
    }
    if (options.max_total_idle_workers == 0U || options.max_total_idle_workers > 4'096U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server total worker limit is invalid");
    }
    return common::Result<void>::success();
}

[[nodiscard]] bool ascii_equal_ignoring_case(const std::string_view left,
                                             const std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(), [](char first, char second) {
        const auto lower = [](const unsigned char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A'))
                                                : value;
        };
        return lower(static_cast<unsigned char>(first)) ==
               lower(static_cast<unsigned char>(second));
    });
}

[[nodiscard]] bool asn1_matches(const ASN1_STRING* value, const std::string_view expected,
                                const bool ignore_ascii_case) noexcept {
    if (value == nullptr) {
        return false;
    }
    const int raw_length = ASN1_STRING_length(value);
    const unsigned char* bytes = ASN1_STRING_get0_data(value);
    if (raw_length < 0 || bytes == nullptr) {
        return false;
    }
    const auto length = static_cast<std::size_t>(raw_length);
    if (length != expected.size() || std::memchr(bytes, '\0', length) != nullptr) {
        return false;
    }
    const std::string_view actual{reinterpret_cast<const char*>(bytes), length};
    return ignore_ascii_case ? ascii_equal_ignoring_case(actual, expected) : actual == expected;
}

[[nodiscard]] bool certificate_san_matches(X509* certificate,
                                           const std::string_view expected) noexcept {
    GENERAL_NAMES* raw_names = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr));
    if (raw_names == nullptr) {
        return false;
    }
    const std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)> names{raw_names,
                                                                              &GENERAL_NAMES_free};
    const int count = sk_GENERAL_NAME_num(names.get());
    for (int index = 0; index < count; ++index) {
        const GENERAL_NAME* name = sk_GENERAL_NAME_value(names.get(), index);
        if (name == nullptr) {
            continue;
        }
        if (expected.starts_with("DNS:") && name->type == GEN_DNS &&
            asn1_matches(name->d.dNSName, expected.substr(4U), true)) {
            return true;
        }
        if (expected.starts_with("URI:") && name->type == GEN_URI &&
            asn1_matches(name->d.uniformResourceIdentifier, expected.substr(4U), false)) {
            return true;
        }
        if (expected.starts_with("EMAIL:") && name->type == GEN_EMAIL &&
            asn1_matches(name->d.rfc822Name, expected.substr(6U), false)) {
            return true;
        }
        if (expected.starts_with("IP:") && name->type == GEN_IPADD) {
            asio::error_code error;
            const auto address = asio::ip::make_address(expected.substr(3U), error);
            const ASN1_OCTET_STRING* value = name->d.iPAddress;
            if (error || value == nullptr) {
                continue;
            }
            if (address.is_v4()) {
                const auto bytes = address.to_v4().to_bytes();
                if (value->length == static_cast<int>(bytes.size()) &&
                    CRYPTO_memcmp(value->data, bytes.data(), bytes.size()) == 0) {
                    return true;
                }
            } else {
                const auto bytes = address.to_v6().to_bytes();
                if (value->length == static_cast<int>(bytes.size()) &&
                    CRYPTO_memcmp(value->data, bytes.data(), bytes.size()) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

[[nodiscard]] bool certificate_matches(protocol::TlsStream& stream,
                                       const ClientCertificateBinding& binding) noexcept {
    if (binding.kind == ClientCertificateBindingKind::none) {
        return true;
    }
    if (SSL_get_verify_result(stream.native_handle()) != X509_V_OK) {
        return false;
    }
    X509* raw_certificate = SSL_get1_peer_certificate(stream.native_handle());
    if (raw_certificate == nullptr) {
        return false;
    }
    const std::unique_ptr<X509, decltype(&X509_free)> certificate{raw_certificate, &X509_free};
    if (binding.kind == ClientCertificateBindingKind::san) {
        return certificate_san_matches(certificate.get(), binding.value);
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0U;
    if (X509_digest(certificate.get(), EVP_sha256(), digest.data(), &digest_size) != 1 ||
        digest_size != 32U) {
        return false;
    }
    std::string actual;
    actual.reserve(64U);
    constexpr std::string_view digits{"0123456789abcdef"};
    for (unsigned int index = 0U; index < digest_size; ++index) {
        actual.push_back(digits[digest[index] >> 4U]);
        actual.push_back(digits[digest[index] & 0x0fU]);
    }
    common::secure_erase_memory(digest.data(), digest.size());
    return actual == binding.value;
}

} // namespace

class Server::Impl final : public std::enable_shared_from_this<Server::Impl> {
  public:
    [[nodiscard]] static common::Result<std::shared_ptr<Impl>> create(asio::io_context& io_context,
                                                                      ServerOptions options) {
        auto valid = validate_options(options);
        if (!valid) {
            return common::Result<std::shared_ptr<Impl>>::failure(valid.error());
        }
        auto endpoint = common::Endpoint::parse(options.listen_endpoint);
        if (!endpoint) {
            return common::Result<std::shared_ptr<Impl>>::failure(endpoint.error());
        }
        auto allowed_ports = common::PortRange::parse(options.allowed_ports);
        if (!allowed_ports) {
            return common::Result<std::shared_ptr<Impl>>::failure(allowed_ports.error());
        }
        asio::error_code address_error;
        auto address = asio::ip::make_address(endpoint->host(), address_error);
        if (address_error) {
            return common::Result<std::shared_ptr<Impl>>::failure(
                common::ErrorCode::invalid_argument,
                "server listen host must be a numeric IPv4 or IPv6 address");
        }
        auto tls_context = protocol::make_server_tls_context({
            .certificate_chain_path = options.tls_certificate_path,
            .private_key_path = options.tls_private_key_path,
            .client_ca_certificate_path = options.client_ca_path,
        });
        if (!tls_context) {
            return common::Result<std::shared_ptr<Impl>>::failure(tls_context.error());
        }
        auto client_policies = ClientPolicyStore::open(
            options.clients_config_path,
            {
                .max_clients = options.max_clients,
                .max_tunnels_per_client = options.max_tunnels_per_client,
                .max_connections_per_client = options.max_connections_per_client,
                .max_idle_workers_per_client = options.max_idle_workers,
            });
        if (!client_policies) {
            return common::Result<std::shared_ptr<Impl>>::failure(client_policies.error());
        }
        if ((*client_policies)->snapshot()->has_certificate_bindings() &&
            options.client_ca_path.empty()) {
            return common::Result<std::shared_ptr<Impl>>::failure(
                common::ErrorCode::invalid_argument,
                "client-ca is required when a client policy binds a certificate");
        }
        auto server_id = common::Id::generate(common::IdKind::server);
        if (!server_id) {
            return common::Result<std::shared_ptr<Impl>>::failure(server_id.error());
        }

        return std::shared_ptr<Impl>{new Impl(io_context, std::move(options),
                                              asio::ip::tcp::endpoint{address, endpoint->port()},
                                              std::move(*tls_context), std::move(*client_policies),
                                              server_id->str(), *allowed_ports)};
    }

    ~Impl() noexcept { stop(); }

    [[nodiscard]] common::Result<void> start() {
        if (running_.exchange(true)) {
            return common::Result<void>::failure(common::ErrorCode::already_exists,
                                                 "TLS server is already running");
        }
        shutting_down_ = false;
        accept_retry_policy_.reset();
        reserved_descriptor_.reopen();

        asio::error_code error;
        acceptor_.open(listen_endpoint_.protocol(), error);
        if (!error) {
            acceptor_.set_option(asio::socket_base::reuse_address{true}, error);
        }
        if (!error) {
            acceptor_.bind(listen_endpoint_, error);
        }
        if (!error) {
            acceptor_.listen(asio::socket_base::max_listen_connections, error);
        }
        if (error) {
            running_.store(false);
            asio::error_code ignored;
            acceptor_.close(ignored);
            return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                 "failed to bind the TLS server listener");
        }

        listening_port_.store(acceptor_.local_endpoint(error).port());
        if (error) {
            running_.store(false);
            acceptor_.close(error);
            return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                 "failed to inspect the TLS server listener");
        }
        warn_file_descriptor_budget();
        accept_next();
        return common::Result<void>::success();
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }
        auto self = shared_from_this();
        asio::dispatch(strand_, [self] { self->begin_shutdown(); });
    }

    [[nodiscard]] common::Result<void> reload() {
        auto context = protocol::make_server_tls_context({
            .certificate_chain_path = options_.tls_certificate_path,
            .private_key_path = options_.tls_private_key_path,
            .client_ca_certificate_path = options_.client_ca_path,
        });
        if (!context) {
            policy_reload_failures_total_.fetch_add(1U, std::memory_order_relaxed);
            return context.error();
        }
        auto changed_clients =
            client_policies_->reload([this](const ClientPolicySnapshot& snapshot) {
                if (snapshot.has_certificate_bindings() && options_.client_ca_path.empty()) {
                    return common::Result<void>::failure(
                        common::ErrorCode::invalid_argument,
                        "client-ca is required when a client policy binds a certificate");
                }
                return common::Result<void>::success();
            });
        if (!changed_clients) {
            policy_reload_failures_total_.fetch_add(1U, std::memory_order_relaxed);
            return changed_clients.error();
        }
        if (!running_.load()) {
            policy_reload_failures_total_.fetch_add(1U, std::memory_order_relaxed);
            return common::Error{common::ErrorCode::connection_failed, "TLS server is not running"};
        }
        auto self = shared_from_this();
        asio::post(strand_, [self, context = std::move(*context),
                             changed_clients = std::move(*changed_clients)]() mutable {
            self->tls_context_ = std::move(context);
            for (const auto& client_id : changed_clients) {
                self->worker_pool_.remove_client(client_id);
                self->tunnel_registry_.remove_client(client_id);
            }
            self->refresh_resource_gauges();
            const auto affected =
                std::make_shared<const std::vector<std::string>>(std::move(changed_clients));
            for (auto& [key, session] : self->sessions_) {
                static_cast<void>(key);
                session->request_reload_stop(affected);
            }
            common::log_info("TLS credentials and client policies reloaded",
                             {.component = "server.audit", .server_id = self->server_id_});
        });
        policy_reloads_total_.fetch_add(1U, std::memory_order_relaxed);
        return common::Result<void>::success();
    }

    [[nodiscard]] std::uint16_t listening_port() const noexcept { return listening_port_.load(); }

    [[nodiscard]] const std::string& server_id() const noexcept { return server_id_; }

    [[nodiscard]] ServerMetrics metrics() const noexcept {
        return {
            .active_sessions = session_registry_.size(),
            .active_connections = active_connections_.load(std::memory_order_relaxed),
            .active_tunnels = active_tunnels_.load(std::memory_order_relaxed),
            .idle_workers = idle_workers_.load(std::memory_order_relaxed),
            .active_relays = active_relays_.load(std::memory_order_relaxed),
            .pending_connections = pending_connection_count_.load(std::memory_order_relaxed),
            .connections_total = connections_total_.load(std::memory_order_relaxed),
            .tls_resumptions_total = tls_resumptions_total_.load(std::memory_order_relaxed),
            .authentication_success_total =
                authentication_success_total_.load(std::memory_order_relaxed),
            .authentication_failure_total =
                authentication_failure_total_.load(std::memory_order_relaxed),
            .registration_success_total =
                registration_success_total_.load(std::memory_order_relaxed),
            .registration_failure_total =
                registration_failure_total_.load(std::memory_order_relaxed),
            .unregistration_total = unregistration_total_.load(std::memory_order_relaxed),
            .relay_total = relay_total_.load(std::memory_order_relaxed),
            .relay_bytes_in_total = relay_bytes_in_total_.load(std::memory_order_relaxed),
            .relay_bytes_out_total = relay_bytes_out_total_.load(std::memory_order_relaxed),
            .acl_rejections_total = acl_rejections_total_.load(std::memory_order_relaxed),
            .quota_rejections_total = quota_rejections_total_.load(std::memory_order_relaxed),
            .source_rejections_total =
                source_rejections_total_.load(std::memory_order_relaxed),
            .errors_total = errors_total_.load(std::memory_order_relaxed),
            .policy_reloads_total = policy_reloads_total_.load(std::memory_order_relaxed),
            .policy_reload_failures_total =
                policy_reload_failures_total_.load(std::memory_order_relaxed),
            .registration_latency_microseconds_total =
                registration_latency_microseconds_total_.load(std::memory_order_relaxed),
        };
    }

  private:
    template <typename Function, typename CompletionToken>
    auto async_run_on_control_strand(Function function, CompletionToken&& token) {
        using Return = std::invoke_result_t<Function>;
        // Keep owning operation state outside Asio's coroutine frame. GCC 11
        // may otherwise duplicate moved lambda state while adapting use_awaitable.
        auto operation = std::make_shared<Function>(std::move(function));
        return asio::async_initiate<CompletionToken, void(Return)>(
            [this, operation](auto handler) mutable {
                auto completion_executor = asio::get_associated_executor(handler);
                asio::post(strand_,
                           [operation, handler = std::move(handler),
                            completion_executor = std::move(completion_executor)]() mutable {
                               auto result = std::make_shared<Return>(std::invoke(*operation));
                               asio::post(std::move(completion_executor),
                                          [handler = std::move(handler),
                                           result = std::move(result)]() mutable {
                                              handler(std::move(*result));
                                          });
                           });
            },
            std::forward<CompletionToken>(token));
    }

    [[nodiscard]] asio::awaitable<common::Result<std::uint64_t>>
    open_client_session(std::shared_ptr<const ClientPolicy> policy) {
        return async_run_on_control_strand(
            [this, policy = std::move(policy)]() mutable {
                const auto current = client_policies_->find(policy->client_id);
                if (current == nullptr || !current->enabled ||
                    current->revision_fingerprint != policy->revision_fingerprint) {
                    return common::Result<std::uint64_t>::failure(
                        common::ErrorCode::authentication_failed,
                        "client policy changed during authentication");
                }
                auto generation = session_registry_.open(policy->client_id);
                if (generation) {
                    worker_pool_.remove_client(policy->client_id);
                    tunnel_registry_.remove_client(policy->client_id);
                    refresh_resource_gauges();
                } else if (generation.error().code() == common::ErrorCode::resource_exhausted) {
                    quota_rejections_total_.fetch_add(1U, std::memory_order_relaxed);
                }
                return generation;
            },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<common::Result<void>>
    register_worker(std::shared_ptr<const ClientPolicy> policy, WorkerRegistration registration,
                    WorkerAssignmentHandler assignment_handler,
                    WorkerRemovalHandler removal_handler) {
        return async_run_on_control_strand(
            [this, policy = std::move(policy), registration, assignment_handler,
             removal_handler]() mutable {
                const auto current = client_policies_->find(policy->client_id);
                if (current == nullptr || !current->enabled ||
                    current->revision_fingerprint != policy->revision_fingerprint) {
                    return common::Result<void>::failure(
                        common::ErrorCode::authentication_failed,
                        "client policy changed before worker registration");
                }
                registration.max_idle_workers = current->max_idle_workers;
                auto result =
                    worker_pool_.add(std::move(registration), std::move(assignment_handler),
                                     std::move(removal_handler));
                if (!result && result.error().code() == common::ErrorCode::resource_exhausted) {
                    quota_rejections_total_.fetch_add(1U, std::memory_order_relaxed);
                }
                refresh_resource_gauges();
                return result;
            },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<bool> remove_worker(const std::string& worker_id) {
        return async_run_on_control_strand(
            [this, worker_id] {
                worker_pool_.remove(worker_id);
                refresh_resource_gauges();
                return true;
            },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<std::size_t> idle_worker_count(const std::string& client_id,
                                                                 const std::uint64_t generation) {
        return async_run_on_control_strand(
            [this, client_id, generation] {
                return worker_pool_.idle_count(client_id, generation);
            },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<common::Result<void>>
    register_tunnel(std::shared_ptr<const ClientPolicy> policy, const TunnelBinding& binding) {
        return async_run_on_control_strand(
            [this, policy = std::move(policy), binding] {
                const auto started = std::chrono::steady_clock::now();
                const auto current = client_policies_->find(policy->client_id);
                if (current == nullptr || !current->enabled ||
                    current->revision_fingerprint != policy->revision_fingerprint) {
                    registration_failure_total_.fetch_add(1U, std::memory_order_relaxed);
                    return common::Result<void>::failure(
                        common::ErrorCode::authentication_failed,
                        "client policy changed before tunnel registration");
                }
                if (!current->allows_port(binding.bind_port)) {
                    acl_rejections_total_.fetch_add(1U, std::memory_order_relaxed);
                    registration_failure_total_.fetch_add(1U, std::memory_order_relaxed);
                    common::log_warn("client ACL rejected a tunnel registration",
                                     {.component = "server.audit",
                                      .server_id = server_id_,
                                      .tunnel_id = binding.tunnel_id,
                                      .error_code = common::ErrorCode::permission_denied});
                    return common::Result<void>::failure(
                        common::ErrorCode::permission_denied,
                        "remote tunnel port is outside the client allowlist");
                }
                auto result = tunnel_registry_.register_tunnel(binding, current->max_tunnels);
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started);
                registration_latency_microseconds_total_.fetch_add(
                    static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed.count(), 0)),
                    std::memory_order_relaxed);
                if (result) {
                    registration_success_total_.fetch_add(1U, std::memory_order_relaxed);
                    common::log_info("client tunnel registered", {.component = "server.audit",
                                                                  .server_id = server_id_,
                                                                  .tunnel_id = binding.tunnel_id});
                } else {
                    registration_failure_total_.fetch_add(1U, std::memory_order_relaxed);
                    if (result.error().code() == common::ErrorCode::resource_exhausted) {
                        quota_rejections_total_.fetch_add(1U, std::memory_order_relaxed);
                    }
                }
                refresh_resource_gauges();
                return result;
            },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<bool> unregister_tunnel(const std::string& client_id,
                                                          const std::uint64_t generation,
                                                          const std::string& tunnel_id,
                                                          const std::uint64_t config_revision) {
        return async_run_on_control_strand(
            [this, client_id, generation, tunnel_id, config_revision] {
                tunnel_registry_.unregister_tunnel(client_id, generation, tunnel_id,
                                                   config_revision);
                unregistration_total_.fetch_add(1U, std::memory_order_relaxed);
                refresh_resource_gauges();
                common::log_info(
                    "client tunnel unregistered",
                    {.component = "server.audit", .server_id = server_id_, .tunnel_id = tunnel_id});
                return true;
            },
            asio::use_awaitable);
    }

    class ControlSession final : public std::enable_shared_from_this<ControlSession> {
      public:
        ControlSession(asio::ip::tcp::socket socket, std::shared_ptr<Impl> server)
            : server_(std::move(server)), tls_context_owner_(server_->tls_context_),
              stream_(std::move(socket), *tls_context_owner_),
              operation_timer_(stream_.get_executor()), heartbeat_timer_(stream_.get_executor()),
              reload_drain_timer_(stream_.get_executor()) {
            asio::error_code endpoint_error;
            const auto endpoint = stream_.lowest_layer().remote_endpoint(endpoint_error);
            remote_endpoint_ = endpoint_error ? std::string{} : endpoint.address().to_string();
            auto connection_id = common::Id::generate(common::IdKind::connection);
            if (connection_id) {
                connection_id_ = connection_id->str();
            }
        }

        ~ControlSession() { protocol::close_tls_stream(stream_); }

        void start() {
            auto self = shared_from_this();
            asio::co_spawn(
                stream_.get_executor(), run(), [self](const std::exception_ptr& failure) {
                    if (failure) {
                        common::log_warn("remote control session ended with an exception",
                                         self->log_context(common::ErrorCode::internal_error));
                    }
                    self->run_finished_ = true;
                    self->cancel_timers();
                    if (!self->goaway_in_progress_) {
                        self->close_transport();
                        self->notify_finished();
                    }
                });
        }

        void request_stop(const bool graceful) {
            auto self = shared_from_this();
            asio::dispatch(stream_.get_executor(),
                           [self, graceful] { self->request_stop_on_executor(graceful); });
        }

        void request_reload_stop(std::shared_ptr<const std::vector<std::string>> affected_clients) {
            auto self = shared_from_this();
            asio::dispatch(
                stream_.get_executor(), [self, affected_clients = std::move(affected_clients)] {
                    if (std::find(affected_clients->begin(), affected_clients->end(),
                                  self->client_id_) == affected_clients->end()) {
                        return;
                    }
                    // A policy change stops listeners and idle capacity immediately,
                    // while an already assigned relay receives the same bounded drain
                    // period used for process shutdown.
                    if (self->worker_assignment_ != nullptr) {
                        self->reload_drain_timer_.expires_after(
                            self->server_->options_.graceful_shutdown_timeout);
                        const auto weak = self->weak_from_this();
                        self->reload_drain_timer_.async_wait([weak](const asio::error_code& error) {
                            if (!error) {
                                if (auto active = weak.lock()) {
                                    active->force_stop_on_executor();
                                }
                            }
                        });
                        return;
                    }
                    self->request_stop_on_executor(true);
                });
        }

        void force_stop() noexcept {
            auto self = shared_from_this();
            asio::dispatch(stream_.get_executor(), [self] { self->force_stop_on_executor(); });
        }

        [[nodiscard]] bool relay_active() const noexcept { return relay_active_.load(); }

        [[nodiscard]] asio::any_io_executor executor() noexcept { return stream_.get_executor(); }

        [[nodiscard]] bool is_control_session(const std::string_view client_id,
                                              const std::uint64_t generation) const noexcept {
            return control_connection_ && client_id_ == client_id && generation_ == generation;
        }

        [[nodiscard]] protocol::CapabilitySet selected_capabilities() const noexcept {
            return selected_capabilities_;
        }

        void enqueue_worker_request(const std::uint16_t count) {
            if (count == 0U) {
                return;
            }
            auto self = shared_from_this();
            asio::post(stream_.get_executor(), [self, count] {
                if (self->run_finished_ || self->stop_requested_ || !self->control_connection_ ||
                    !self->state_.has_value() ||
                    self->state_->state() != protocol::ConnectionState::authenticated) {
                    return;
                }
                const auto available = static_cast<std::size_t>(kMaxWorkerRequestCount) -
                                       std::min<std::size_t>(self->pending_worker_request_count_,
                                                             kMaxWorkerRequestCount);
                self->pending_worker_request_count_ = std::min<std::size_t>(
                    kMaxWorkerRequestCount,
                    self->pending_worker_request_count_ + std::min<std::size_t>(count, available));
                self->worker_request_wakeup_ = self->pending_worker_request_count_ != 0U;
                if (self->worker_request_wakeup_) {
                    static_cast<void>(self->heartbeat_timer_.cancel());
                }
            });
        }

        void cleanup_on_control_strand() noexcept {
            if (cleanup_complete_) {
                return;
            }
            cleanup_complete_ = true;
            if (worker_registered_) {
                server_->worker_pool_.remove(worker_id_);
                worker_registered_ = false;
            }
            if (control_connection_ && generation_ != 0U && !client_id_.empty()) {
                server_->worker_pool_.remove_session(client_id_, generation_);
                server_->tunnel_registry_.remove_session(client_id_, generation_);
                server_->cancel_pending_for_session(client_id_, generation_);
                server_->session_registry_.close(client_id_, generation_);
                server_->worker_request_states_.erase(pending_session_key(client_id_, generation_));
            }
            server_->refresh_resource_gauges();
            server_->active_connections_.fetch_sub(1U);
        }

      private:
        void request_stop_on_executor(const bool graceful) {
            if (stop_requested_) {
                return;
            }
            stop_requested_ = true;
            if (graceful && control_connection_ && generation_ != 0U && state_.has_value() &&
                state_->state() == protocol::ConnectionState::authenticated &&
                !write_in_progress_) {
                send_goaway();
                return;
            }
            force_stop_on_executor();
        }

        void force_stop_on_executor() noexcept {
            stop_requested_ = true;
            cancel_timers();
            close_transport();
        }
        struct WorkerAssignment final {
            TunnelBinding binding;
            asio::ip::tcp::socket public_socket;
            ConnectionQuota::Lease connection_lease;
        };

        [[nodiscard]] asio::awaitable<void> run() {
            const bool handshake_complete = co_await perform_tls_handshake();
            if (!handshake_complete) {
                co_return;
            }
            auto first_frame = co_await read_initial_frame();
            if (!first_frame) {
                co_return;
            }
            if (first_frame->type == protocol::MessageType::worker_hello) {
                state_.emplace(protocol::PeerRole::server, protocol::ConnectionKind::worker);
                if (!state_->on_receive(first_frame->type)) {
                    co_return;
                }
                co_await run_worker(*first_frame);
                co_return;
            }
            if (first_frame->type != protocol::MessageType::hello) {
                co_return;
            }
            control_connection_ = true;
            state_.emplace(protocol::PeerRole::server, protocol::ConnectionKind::control);
            const auto transition = state_->on_receive(first_frame->type);
            if (!transition) {
                co_return;
            }
            const bool authenticated = co_await authenticate(*first_frame);
            if (!authenticated) {
                server_->authentication_failure_total_.fetch_add(1U, std::memory_order_relaxed);
                co_return;
            }
            server_->authentication_success_total_.fetch_add(1U, std::memory_order_relaxed);
            common::log_info("remote client authenticated", audit_context());
            co_await heartbeat_loop();
        }

        [[nodiscard]] asio::awaitable<bool> perform_tls_handshake() {
            arm_operation_timeout(server_->options_.handshake_timeout);
            asio::error_code error;
            co_await stream_.async_handshake(asio::ssl::stream_base::server,
                                             asio::redirect_error(asio::use_awaitable, error));
            cancel_operation_timeout();
            if (error) {
                server_->errors_total_.fetch_add(1U, std::memory_order_relaxed);
                common::log_warn("TLS handshake failed", log_context(common::ErrorCode::tls_error));
                co_return false;
            }
            if (protocol::tls_session_reused(stream_)) {
                server_->tls_resumptions_total_.fetch_add(1U, std::memory_order_relaxed);
            }
            co_return true;
        }

        [[nodiscard]] asio::awaitable<bool> authenticate(const protocol::Frame& hello_frame) {
            auto hello = protocol::decode_hello(hello_frame.payload);
            if (!hello) {
                co_return false;
            }
            client_id_ = hello->client_id;
            policy_owner_ = server_->client_policies_->find(client_id_);
            const bool policy_eligible = policy_owner_ != nullptr && policy_owner_->enabled &&
                                         policy_owner_->psk != nullptr &&
                                         certificate_matches(stream_, policy_owner_->certificate);
            selected_capabilities_ = hello->capabilities & protocol::kSupportedCapabilities;
            if ((selected_capabilities_ & protocol::kRequiredCapabilities) !=
                protocol::kRequiredCapabilities) {
                co_return false;
            }

            auto nonce = protocol::generate_authentication_nonce();
            if (!nonce) {
                co_return false;
            }
            challenge_nonce_ = *nonce;
            auto ack_payload = protocol::encode_hello_ack({
                .server_id = server_->server_id_,
                .server_time_seconds = common::unix_seconds_now(),
                .nonce = challenge_nonce_,
                .selected_capabilities = selected_capabilities_,
            });
            if (!ack_payload) {
                co_return false;
            }
            const protocol::Frame hello_ack_frame{protocol::MessageType::hello_ack, 0U,
                                                  hello_frame.request_id, std::move(*ack_payload)};
            if (!co_await write_frame(hello_ack_frame, server_->options_.handshake_timeout)) {
                co_return false;
            }

            auto auth_frame = co_await read_frame(server_->options_.handshake_timeout);
            if (!auth_frame || auth_frame->type != protocol::MessageType::auth) {
                co_return false;
            }
            auto auth = protocol::decode_auth(auth_frame->payload);
            if (!auth) {
                co_return co_await reject_authentication(auth_frame->request_id);
            }

            const auto now = std::chrono::steady_clock::now();
            bool accepted = server_->auth_rate_limiter_.allowed(remote_endpoint_, now);
            accepted = accepted && policy_eligible;
            accepted = accepted && auth->client_id == client_id_;
            accepted = accepted && auth->nonce == challenge_nonce_;
            accepted = accepted && auth->selected_capabilities == selected_capabilities_;
            if (accepted) {
                auto skew = common::is_clock_skew_within(auth->timestamp_seconds,
                                                         common::unix_seconds_now(),
                                                         server_->options_.allowed_clock_skew);
                accepted = skew && *skew;
            }
            // Always perform the same HMAC path for a syntactically valid AUTH.
            // Unknown, disabled, and certificate-mismatched clients use an
            // ephemeral rejection key so policy existence is not exposed by an
            // avoidable authentication timing distinction.
            const std::string_view psk =
                policy_eligible ? policy_owner_->psk->view() : server_->rejection_psk_.view();
            auto verified = protocol::verify_and_consume_authentication_data(
                server_->nonce_cache_, psk, auth->client_id, server_->server_id_,
                auth->timestamp_seconds, auth->nonce, auth->selected_capabilities,
                auth->authentication_data, now);
            accepted = accepted && verified && *verified;
            if (!accepted) {
                server_->auth_rate_limiter_.record_failure(remote_endpoint_, now);
                co_return co_await reject_authentication(auth_frame->request_id);
            }

            auto generation = co_await server_->open_client_session(policy_owner_);
            if (!generation) {
                auto error_payload =
                    protocol::encode_auth_error({common::ErrorCode::resource_exhausted});
                if (error_payload) {
                    const protocol::Frame auth_error_frame{protocol::MessageType::auth_error, 0U,
                                                           auth_frame->request_id,
                                                           std::move(*error_payload)};
                    static_cast<void>(co_await write_frame(auth_error_frame,
                                                           server_->options_.handshake_timeout));
                }
                co_return false;
            }
            generation_ = *generation;

            const auto heartbeat_milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    server_->options_.heartbeat_interval);
            const auto policy_max_idle =
                static_cast<std::uint16_t>(policy_owner_->max_idle_workers);
            auto ok_payload = protocol::encode_auth_ok({
                .session_generation = generation_,
                .heartbeat_interval_milliseconds =
                    static_cast<std::uint32_t>(heartbeat_milliseconds.count()),
                .min_idle_workers = std::min(server_->options_.min_idle_workers, policy_max_idle),
                .max_idle_workers = policy_max_idle,
            });
            if (!ok_payload) {
                co_return false;
            }
            const protocol::Frame auth_ok_frame{protocol::MessageType::auth_ok, 0U,
                                                auth_frame->request_id, std::move(*ok_payload)};
            if (!co_await write_frame(auth_ok_frame, server_->options_.handshake_timeout)) {
                co_return false;
            }
            server_->auth_rate_limiter_.record_success(remote_endpoint_);
            co_return true;
        }

        [[nodiscard]] asio::awaitable<bool> reject_authentication(const std::uint64_t request_id) {
            auto payload = protocol::encode_auth_error({common::ErrorCode::authentication_failed});
            if (payload) {
                const protocol::Frame auth_error_frame{protocol::MessageType::auth_error, 0U,
                                                       request_id, std::move(*payload)};
                static_cast<void>(
                    co_await write_frame(auth_error_frame, server_->options_.handshake_timeout));
            }
            common::log_warn("remote client authentication failed",
                             audit_context(common::ErrorCode::authentication_failed));
            co_return false;
        }

        [[nodiscard]] asio::awaitable<void> run_worker(const protocol::Frame& hello_frame) {
            auto hello = protocol::decode_worker_hello(hello_frame.payload);
            if (!hello) {
                co_return;
            }
            policy_owner_ = server_->client_policies_->find(hello->client_id);
            if (policy_owner_ == nullptr || !policy_owner_->enabled ||
                !certificate_matches(stream_, policy_owner_->certificate) ||
                !server_->session_registry_.is_current(hello->client_id,
                                                       hello->session_generation)) {
                co_return;
            }
            auto skew =
                common::is_clock_skew_within(hello->timestamp_seconds, common::unix_seconds_now(),
                                             server_->options_.allowed_clock_skew);
            if (!skew || !*skew) {
                co_return;
            }
            auto verified = protocol::verify_and_consume_worker_authentication_data(
                server_->nonce_cache_, policy_owner_->psk->view(), hello->client_id,
                server_->server_id_, hello->session_generation, hello->worker_id,
                hello->timestamp_seconds, hello->nonce, hello->authentication_data);
            if (!verified || !*verified) {
                co_return;
            }
            client_id_ = hello->client_id;
            generation_ = hello->session_generation;
            worker_id_ = hello->worker_id;
            selected_capabilities_ =
                co_await server_->control_session_capabilities(client_id_, generation_);

            auto accepted_payload = protocol::encode_worker_accepted({worker_id_});
            if (!accepted_payload) {
                co_return;
            }
            const protocol::Frame accepted_frame{protocol::MessageType::worker_accepted, 0U,
                                                 hello_frame.request_id,
                                                 std::move(*accepted_payload)};
            if (!co_await write_frame(accepted_frame, server_->options_.handshake_timeout)) {
                co_return;
            }

            // Named lvalues force independent ownership before this coroutine
            // suspends, including with GCC 11 and Asio 1.18.
            const auto weak = weak_from_this();
            const WorkerRegistration registration{client_id_, generation_, worker_id_};
            const WorkerAssignmentHandler assignment_handler =
                [weak](TunnelBinding binding, asio::ip::tcp::socket public_socket,
                       ConnectionQuota::Lease connection_lease) mutable {
                    if (auto self = weak.lock()) {
                        asio::post(self->stream_.get_executor(),
                                   [self, binding = std::move(binding),
                                    public_socket = std::move(public_socket),
                                    connection_lease = std::move(connection_lease)]() mutable {
                                       if (self->run_finished_ || self->stop_requested_) {
                                           asio::error_code ignored;
                                           public_socket.close(ignored);
                                           return;
                                       }
                                       self->worker_registered_ = false;
                                       self->worker_assignment_ =
                                           std::make_unique<WorkerAssignment>(WorkerAssignment{
                                               std::move(binding), std::move(public_socket),
                                               std::move(connection_lease)});
                                       try {
                                           static_cast<void>(self->heartbeat_timer_.cancel());
                                       } catch (...) {
                                       }
                                   });
                    } else {
                        asio::error_code ignored;
                        public_socket.close(ignored);
                    }
                };
            const WorkerRemovalHandler removal_handler = [weak] {
                if (auto self = weak.lock()) {
                    asio::post(self->stream_.get_executor(), [self] {
                        self->worker_registered_ = false;
                        self->force_stop_on_executor();
                    });
                }
            };
            auto registered = co_await server_->register_worker(
                policy_owner_, registration, assignment_handler, removal_handler);
            if (!registered) {
                co_return;
            }
            worker_registered_ = true;
            server_->worker_request_registered(client_id_, generation_);

            heartbeat_timer_.expires_after(server_->options_.worker_idle_timeout);
            server_->notify_worker_available(client_id_, generation_);
            asio::error_code idle_error;
            co_await heartbeat_timer_.async_wait(
                asio::redirect_error(asio::use_awaitable, idle_error));
            if (worker_assignment_ == nullptr) {
                static_cast<void>(co_await server_->remove_worker(worker_id_));
                worker_registered_ = false;
                co_return;
            }
            co_await handle_worker_assignment();
        }

        [[nodiscard]] asio::awaitable<void> handle_worker_assignment() {
            if (worker_assignment_ == nullptr) {
                co_return;
            }
            auto connection_id = common::Id::generate(common::IdKind::connection);
            if (!connection_id) {
                co_return;
            }
            const std::string connection_id_text = connection_id->str();
            std::optional<std::string> source_host;
            std::optional<std::uint16_t> source_port;
            const bool supports_source_endpoint =
                (selected_capabilities_ &
                 static_cast<protocol::CapabilitySet>(protocol::Capability::proxy_protocol)) != 0U ||
                (selected_capabilities_ & static_cast<protocol::CapabilitySet>(
                                              protocol::Capability::tcp_simultaneous_open)) != 0U;
            if (supports_source_endpoint) {
                asio::error_code source_error;
                const auto source_endpoint =
                    worker_assignment_->public_socket.remote_endpoint(source_error);
                if (!source_error) {
                    source_host = source_endpoint.address().to_string();
                    source_port = source_endpoint.port();
                }
            }
            auto relay_payload = protocol::encode_start_relay(
                {worker_assignment_->binding.tunnel_id, connection_id_text,
                 worker_assignment_->binding.mode, std::move(source_host), source_port});
            if (!relay_payload) {
                co_return;
            }
            const protocol::Frame relay_frame{protocol::MessageType::start_relay, 0U, 1U,
                                              std::move(*relay_payload)};
            if (!co_await write_frame(relay_frame, server_->options_.handshake_timeout)) {
                co_return;
            }
            auto local_result = co_await read_frame(server_->options_.handshake_timeout);
            if (!local_result) {
                co_return;
            }
            if (local_result->type == protocol::MessageType::local_connect_error) {
                auto failed = protocol::decode_local_connect_error(local_result->payload);
                if (!failed || failed->connection_id != connection_id_text) {
                    co_return;
                }
                co_return;
            }
            if (local_result->type != protocol::MessageType::local_connect_ok) {
                co_return;
            }
            auto connected = protocol::decode_local_connect_ok(local_result->payload);
            if (!connected || connected->connection_id != connection_id_text) {
                co_return;
            }
            relay_active_.store(true);
            server_->active_relays_.fetch_add(1U, std::memory_order_relaxed);
            server_->relay_total_.fetch_add(1U, std::memory_order_relaxed);
            auto relayed = co_await protocol::relay_tls_and_tcp(
                stream_, worker_assignment_->public_socket,
                {.inactivity_timeout = server_->options_.relay_inactivity_timeout});
            relay_active_.store(false);
            server_->active_relays_.fetch_sub(1U, std::memory_order_relaxed);
            if (relayed) {
                server_->relay_bytes_in_total_.fetch_add(relayed->tls_to_tcp_bytes,
                                                         std::memory_order_relaxed);
                server_->relay_bytes_out_total_.fetch_add(relayed->tcp_to_tls_bytes,
                                                          std::memory_order_relaxed);
            }
            if (!relayed && relayed.error().code() != common::ErrorCode::connection_timeout) {
                server_->errors_total_.fetch_add(1U, std::memory_order_relaxed);
                common::log_warn("public relay ended with a transport error",
                                 log_context(relayed.error().code()));
            }
        }

        [[nodiscard]] asio::awaitable<void> heartbeat_loop() {
            std::uint64_t sequence = 1U;
            for (;;) {
                if (!server_->session_registry_.is_current(client_id_, generation_)) {
                    co_return;
                }
                const auto next_heartbeat =
                    std::chrono::steady_clock::now() + server_->options_.heartbeat_interval;
                for (;;) {
                    const IdleWaitResult ready =
                        co_await wait_for_heartbeat_or_input(next_heartbeat);
                    if (ready == IdleWaitResult::stopped) {
                        co_return;
                    }
                    if (ready == IdleWaitResult::worker_request) {
                        if (stop_requested_) {
                            co_return;
                        }
                        const auto requested = take_worker_request();
                        if (requested == 0U) {
                            continue;
                        }
                        auto request_payload = protocol::encode_request_workers({requested});
                        if (!request_payload) {
                            co_return;
                        }
                        const protocol::Frame request_frame{protocol::MessageType::request_workers,
                                                            0U, sequence,
                                                            std::move(*request_payload)};
                        if (!co_await write_frame(request_frame,
                                                  server_->options_.heartbeat_timeout)) {
                            co_return;
                        }
                        advance_sequence(sequence);
                        continue;
                    }
                    if (ready == IdleWaitResult::heartbeat_due) {
                        break;
                    }
                    auto frame = co_await read_frame(server_->options_.heartbeat_timeout);
                    if (!frame || !co_await handle_control_request(
                                      *frame, server_->options_.heartbeat_timeout)) {
                        co_return;
                    }
                }

                const auto missing = co_await server_->reserve_worker_request(
                    client_id_, generation_,
                    std::min<std::size_t>(server_->options_.min_idle_workers,
                                          policy_owner_->max_idle_workers));
                if (missing != 0U) {
                    auto request_payload = protocol::encode_request_workers({missing});
                    if (!request_payload) {
                        co_return;
                    }
                    const protocol::Frame request_frame{protocol::MessageType::request_workers, 0U,
                                                        sequence, std::move(*request_payload)};
                    if (!co_await write_frame(request_frame, server_->options_.heartbeat_timeout)) {
                        co_return;
                    }
                    advance_sequence(sequence);
                }

                auto heartbeat_sequence = protocol::encode_worker_timeout_heartbeat_sequence(
                    sequence,
                    static_cast<std::uint16_t>(server_->options_.worker_idle_timeout.count()));
                if (!heartbeat_sequence) {
                    co_return;
                }
                auto ping_payload = protocol::encode_heartbeat({*heartbeat_sequence});
                if (!ping_payload) {
                    co_return;
                }
                const protocol::Frame ping_frame{protocol::MessageType::ping, 0U,
                                                 *heartbeat_sequence, std::move(*ping_payload)};
                if (!co_await write_frame(ping_frame, server_->options_.heartbeat_timeout)) {
                    co_return;
                }

                const auto deadline =
                    std::chrono::steady_clock::now() + server_->options_.heartbeat_timeout;
                bool received_pong = false;
                while (!received_pong) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        common::log_warn("remote client heartbeat timed out",
                                         log_context(common::ErrorCode::connection_timeout));
                        co_return;
                    }
                    const auto remaining = std::chrono::ceil<std::chrono::seconds>(deadline - now);
                    auto frame = co_await read_frame(remaining);
                    if (!frame) {
                        co_return;
                    }
                    if (!server_->session_registry_.is_current(client_id_, generation_)) {
                        co_return;
                    }
                    if (frame->type == protocol::MessageType::pong) {
                        auto pong = protocol::decode_heartbeat(frame->payload);
                        if (!pong || pong->sequence != *heartbeat_sequence ||
                            frame->request_id != *heartbeat_sequence) {
                            co_return;
                        }
                        received_pong = true;
                        continue;
                    }
                    if (!co_await handle_control_request(*frame, remaining)) {
                        co_return;
                    }
                }
                advance_sequence(sequence);
            }
        }

        enum class IdleWaitResult : std::uint8_t {
            heartbeat_due,
            readable,
            worker_request,
            stopped,
        };

        [[nodiscard]] asio::awaitable<IdleWaitResult>
        wait_for_heartbeat_or_input(const std::chrono::steady_clock::time_point deadline) {
            if (pending_worker_request_count_ != 0U) {
                worker_request_wakeup_ = false;
                co_return IdleWaitResult::worker_request;
            }
            // A previous SSL_read may have pulled several pipelined control frames
            // from the socket into OpenSSL. In that case the kernel descriptor is
            // no longer readable even though another complete request is ready.
            if (SSL_pending(stream_.native_handle()) > 0 ||
                SSL_has_pending(stream_.native_handle()) == 1) {
                co_return IdleWaitResult::readable;
            }
            struct WaitState final {
                bool active{true};
                bool readable{false};
            };

            auto state = std::make_shared<WaitState>();
            auto self = shared_from_this();
            stream_.lowest_layer().async_wait(
                asio::ip::tcp::socket::wait_read, [self, state](const asio::error_code& error) {
                    if (!error && state->active) {
                        state->active = false;
                        state->readable = true;
                        static_cast<void>(self->heartbeat_timer_.cancel());
                    }
                });

            heartbeat_timer_.expires_at(deadline);
            asio::error_code timer_error;
            co_await heartbeat_timer_.async_wait(
                asio::redirect_error(asio::use_awaitable, timer_error));
            state->active = false;
            if (state->readable) {
                co_return IdleWaitResult::readable;
            }
            if (pending_worker_request_count_ != 0U || worker_request_wakeup_) {
                worker_request_wakeup_ = false;
                co_return IdleWaitResult::worker_request;
            }

            // Only the readiness wait is outstanding while the control session is
            // idle. Cancel it before starting the next framed operation; socket
            // cancellation does not close the established TCP connection.
            asio::error_code ignored;
            stream_.lowest_layer().cancel(ignored);
            co_return timer_error ? IdleWaitResult::stopped : IdleWaitResult::heartbeat_due;
        }

        [[nodiscard]] std::uint16_t take_worker_request() noexcept {
            const auto requested = static_cast<std::uint16_t>(
                std::min<std::size_t>(pending_worker_request_count_, kMaxWorkerRequestCount));
            pending_worker_request_count_ -= requested;
            worker_request_wakeup_ = pending_worker_request_count_ != 0U;
            return requested;
        }

        static void advance_sequence(std::uint64_t& sequence) noexcept {
            ++sequence;
            if (sequence == 0U || sequence > protocol::kMaximumNegotiatedHeartbeatSequence) {
                sequence = 1U;
            }
        }

        [[nodiscard]] asio::awaitable<bool>
        handle_control_request(const protocol::Frame& frame, const std::chrono::seconds timeout) {
            if (frame.type == protocol::MessageType::ping) {
                auto ping = protocol::decode_heartbeat(frame.payload);
                if (!ping) {
                    co_return false;
                }
                auto pong_payload = protocol::encode_heartbeat(*ping);
                if (!pong_payload) {
                    co_return false;
                }
                const protocol::Frame pong_frame{protocol::MessageType::pong, 0U, frame.request_id,
                                                 std::move(*pong_payload)};
                co_return co_await write_frame(pong_frame, timeout);
            }
            if (frame.type == protocol::MessageType::register_tunnel) {
                co_return co_await handle_register_tunnel(frame, timeout);
            }
            if (frame.type == protocol::MessageType::unregister_tunnel) {
                co_return co_await handle_unregister_tunnel(frame, timeout);
            }
            co_return false;
        }

        [[nodiscard]] asio::awaitable<bool>
        handle_register_tunnel(const protocol::Frame& frame, const std::chrono::seconds timeout) {
            auto registration = protocol::decode_register_tunnel(frame.payload);
            if (!registration) {
                co_return false;
            }
            if (!protocol::supports_tunnel_mode(selected_capabilities_, registration->mode)) {
                auto payload = protocol::encode_register_tunnel_error(
                    {registration->tunnel_id, common::ErrorCode::unsupported_version,
                     registration->desired_revision});
                if (!payload) {
                    co_return false;
                }
                const protocol::Frame error_frame{protocol::MessageType::register_tunnel_error, 0U,
                                                  frame.request_id, std::move(*payload)};
                co_return co_await write_frame(error_frame, timeout);
            }
            // Keep this aggregate out of the co_await expression so its strings
            // have unambiguous ownership across older coroutine implementations.
            const TunnelBinding binding{
                .client_id = client_id_,
                .session_generation = generation_,
                .tunnel_id = registration->tunnel_id,
                .bind_host = registration->bind_host,
                .bind_port = registration->bind_port,
                .config_revision = registration->desired_revision,
                .mode = registration->mode,
            };
            auto registered = co_await server_->register_tunnel(policy_owner_, binding);
            if (!registered) {
                auto payload = protocol::encode_register_tunnel_error(
                    {registration->tunnel_id, registered.error().code(),
                     registration->desired_revision});
                if (!payload) {
                    co_return false;
                }
                const protocol::Frame error_frame{protocol::MessageType::register_tunnel_error, 0U,
                                                  frame.request_id, std::move(*payload)};
                co_return co_await write_frame(error_frame, timeout);
            }
            common::trigger_failpoint("server.after_listener_established");
            auto payload = protocol::encode_register_tunnel_ok(
                {registration->tunnel_id, registration->desired_revision});
            if (!payload) {
                co_return false;
            }
            const protocol::Frame accepted_frame{protocol::MessageType::register_tunnel_ok, 0U,
                                                 frame.request_id, std::move(*payload)};
            co_return co_await write_frame(accepted_frame, timeout);
        }

        [[nodiscard]] asio::awaitable<bool>
        handle_unregister_tunnel(const protocol::Frame& frame, const std::chrono::seconds timeout) {
            auto removal = protocol::decode_unregister_tunnel(frame.payload);
            if (!removal) {
                co_return false;
            }
            static_cast<void>(co_await server_->unregister_tunnel(
                client_id_, generation_, removal->tunnel_id, removal->desired_revision));
            auto payload = protocol::encode_unregister_tunnel_ok(
                {removal->tunnel_id, removal->desired_revision});
            if (!payload) {
                co_return false;
            }
            const protocol::Frame removed_frame{protocol::MessageType::unregister_tunnel_ok, 0U,
                                                frame.request_id, std::move(*payload)};
            co_return co_await write_frame(removed_frame, timeout);
        }

        [[nodiscard]] asio::awaitable<common::Result<protocol::Frame>> read_initial_frame() {
            arm_operation_timeout(server_->options_.handshake_timeout);
            auto frame = co_await protocol::async_read_frame(stream_);
            cancel_operation_timeout();
            co_return frame;
        }

        [[nodiscard]] asio::awaitable<common::Result<protocol::Frame>>
        read_frame(const std::chrono::seconds timeout) {
            arm_operation_timeout(timeout);
            auto frame = co_await protocol::async_read_frame(stream_);
            cancel_operation_timeout();
            if (!frame) {
                co_return frame;
            }
            if (!state_.has_value()) {
                co_return common::Result<protocol::Frame>::failure(
                    common::ErrorCode::internal_error, "remote connection state is unavailable");
            }
            auto transition = state_->on_receive(frame->type);
            if (!transition) {
                co_return common::Result<protocol::Frame>::failure(transition.error());
            }
            co_return frame;
        }

        [[nodiscard]] asio::awaitable<bool> write_frame(const protocol::Frame& frame,
                                                        const std::chrono::seconds timeout) {
            if (!state_.has_value()) {
                co_return false;
            }
            auto transition = state_->on_send(frame.type);
            if (!transition) {
                co_return false;
            }
            arm_operation_timeout(timeout);
            write_in_progress_ = true;
            auto written = co_await protocol::async_write_frame(stream_, frame);
            write_in_progress_ = false;
            cancel_operation_timeout();
            co_return static_cast<bool>(written);
        }

        void send_goaway() {
            auto transition = state_->on_send(protocol::MessageType::goaway);
            if (!transition) {
                force_stop();
                return;
            }
            goaway_in_progress_ = true;
            auto self = shared_from_this();
            asio::co_spawn(
                stream_.get_executor(),
                protocol::async_write_frame(stream_, {protocol::MessageType::goaway, 0U, 0U, {}}),
                [self](const std::exception_ptr&, const common::Result<void>&) {
                    self->goaway_in_progress_ = false;
                    self->cancel_timers();
                    self->close_transport();
                    if (self->run_finished_) {
                        self->notify_finished();
                    }
                });
        }

        void close_transport() noexcept {
            if (worker_assignment_ != nullptr) {
                asio::error_code ignored;
                worker_assignment_->public_socket.cancel(ignored);
                worker_assignment_->public_socket.close(ignored);
            }
            protocol::close_tls_stream(stream_);
        }

        void notify_finished() noexcept {
            if (finished_notified_) {
                return;
            }
            finished_notified_ = true;
            server_->session_finished(shared_from_this());
        }

        void arm_operation_timeout(const std::chrono::seconds timeout) {
            operation_timer_.expires_after(timeout);
            auto weak = weak_from_this();
            operation_timer_.async_wait([weak](const asio::error_code& error) {
                if (!error) {
                    if (auto self = weak.lock()) {
                        protocol::close_tls_stream(self->stream_);
                    }
                }
            });
        }

        void cancel_operation_timeout() noexcept {
            try {
                static_cast<void>(operation_timer_.cancel());
            } catch (...) {
            }
        }

        void cancel_timers() noexcept {
            try {
                static_cast<void>(operation_timer_.cancel());
                static_cast<void>(heartbeat_timer_.cancel());
                static_cast<void>(reload_drain_timer_.cancel());
            } catch (...) {
            }
        }

        [[nodiscard]] common::LogContext
        log_context(const std::optional<common::ErrorCode> error = std::nullopt) const noexcept {
            return {
                .component = "server.control",
                .server_id = server_->server_id_,
                .connection_id = connection_id_,
                .remote_endpoint = remote_endpoint_,
                .error_code = error,
            };
        }

        [[nodiscard]] common::LogContext
        audit_context(const std::optional<common::ErrorCode> error = std::nullopt) const noexcept {
            return {
                .component = "server.audit",
                .server_id = server_->server_id_,
                .connection_id = connection_id_,
                .remote_endpoint = remote_endpoint_,
                .error_code = error,
            };
        }

        std::shared_ptr<Impl> server_;
        std::shared_ptr<asio::ssl::context> tls_context_owner_;
        std::shared_ptr<const ClientPolicy> policy_owner_;
        protocol::TlsStream stream_;
        asio::steady_timer operation_timer_;
        asio::steady_timer heartbeat_timer_;
        asio::steady_timer reload_drain_timer_;
        std::optional<protocol::StateMachine> state_;
        std::string remote_endpoint_;
        std::string connection_id_;
        std::string client_id_;
        std::string worker_id_;
        protocol::AuthenticationNonce challenge_nonce_{};
        std::uint64_t generation_{0U};
        protocol::CapabilitySet selected_capabilities_{0U};
        std::unique_ptr<WorkerAssignment> worker_assignment_;
        bool control_connection_{false};
        bool worker_registered_{false};
        std::atomic<bool> relay_active_{false};
        bool write_in_progress_{false};
        bool stop_requested_{false};
        bool goaway_in_progress_{false};
        bool run_finished_{false};
        bool finished_notified_{false};
        bool cleanup_complete_{false};
        std::size_t pending_worker_request_count_{0U};
        bool worker_request_wakeup_{false};
    };

    struct WorkerRequestState final {
        std::size_t outstanding{0U};
        std::chrono::steady_clock::time_point last_request{};
    };

    [[nodiscard]] std::optional<std::uint16_t>
    reserve_worker_request_on_control_strand(const std::string_view client_id,
                                             const std::uint64_t generation,
                                             const std::size_t desired) {
        if (desired == 0U) {
            return std::nullopt;
        }
        const auto policy = client_policies_->find(client_id);
        if (policy == nullptr || !policy->enabled) {
            return std::nullopt;
        }
        const std::size_t max_idle_workers =
            std::min(policy->max_idle_workers, static_cast<std::size_t>(options_.max_idle_workers));
        ControlSession* control_session = nullptr;
        for (const auto& [key, session] : sessions_) {
            static_cast<void>(key);
            if (session->is_control_session(client_id, generation)) {
                control_session = session.get();
                break;
            }
        }
        if (control_session == nullptr) {
            worker_request_states_.erase(pending_session_key(client_id, generation));
            return std::nullopt;
        }

        const std::string key = pending_session_key(client_id, generation);
        auto& state = worker_request_states_[key];
        const auto now = std::chrono::steady_clock::now();
        if (state.outstanding != 0U && now - state.last_request >= kWorkerRequestRetryAfter) {
            state.outstanding = 0U;
        }
        if (state.last_request != std::chrono::steady_clock::time_point{} &&
            now - state.last_request < kWorkerRequestCooldown) {
            return std::nullopt;
        }
        const std::size_t idle_workers = worker_pool_.idle_count(client_id, generation);
        const std::size_t accounted = idle_workers + state.outstanding;
        if (desired <= accounted || state.outstanding >= max_idle_workers) {
            return std::nullopt;
        }
        const std::size_t global_capacity =
            options_.max_total_idle_workers > worker_pool_.size()
                ? options_.max_total_idle_workers - worker_pool_.size()
                : 0U;
        const std::size_t per_session_capacity =
            max_idle_workers > idle_workers + state.outstanding
                ? max_idle_workers - idle_workers - state.outstanding
                : 0U;
        const std::size_t requested =
            std::min({desired - accounted, global_capacity, per_session_capacity,
                      static_cast<std::size_t>(kMaxWorkerRequestCount)});
        if (requested == 0U) {
            return std::nullopt;
        }
        state.outstanding += requested;
        state.last_request = now;
        return static_cast<std::uint16_t>(requested);
    }

    [[nodiscard]] asio::awaitable<std::uint16_t>
    reserve_worker_request(std::string client_id, const std::uint64_t generation,
                           const std::size_t desired) {
        return async_run_on_control_strand(
            [this, client_id = std::move(client_id), generation, desired] {
                const auto requested =
                    reserve_worker_request_on_control_strand(client_id, generation, desired);
                return requested.value_or(0U);
            },
            asio::use_awaitable);
    }

    /// Negotiated capabilities of the client's control session; workers adopt
    /// them so extension payloads (source endpoint) are only sent to peers
    /// that advertised the corresponding capabilities.
    [[nodiscard]] asio::awaitable<protocol::CapabilitySet>
    control_session_capabilities(std::string client_id, const std::uint64_t generation) {
        return async_run_on_control_strand(
            [this, client_id = std::move(client_id), generation] {
                for (const auto& [key, session] : sessions_) {
                    static_cast<void>(key);
                    if (session->is_control_session(client_id, generation)) {
                        return session->selected_capabilities();
                    }
                }
                return protocol::CapabilitySet{0U};
            },
            asio::use_awaitable);
    }

    void worker_request_registered(std::string client_id, const std::uint64_t generation) {
        auto self = shared_from_this();
        asio::post(strand_, [self, client_id = std::move(client_id), generation] {
            const std::string key = pending_session_key(client_id, generation);
            const auto state = self->worker_request_states_.find(key);
            if (state != self->worker_request_states_.end() && state->second.outstanding != 0U) {
                --state->second.outstanding;
            }
            self->maybe_request_workers_for_pending(client_id, generation);
        });
    }

    class PendingPublicConnection final
        : public std::enable_shared_from_this<PendingPublicConnection> {
      public:
        PendingPublicConnection(std::shared_ptr<Impl> server, TunnelBinding binding,
                                asio::ip::tcp::socket public_socket,
                                ConnectionQuota::Lease connection_lease)
            : server_(std::move(server)), binding_(std::move(binding)),
              session_key_(pending_session_key(binding_.client_id, binding_.session_generation)),
              public_socket_(std::move(public_socket)), deadline_timer_(server_->strand_),
              connection_lease_(std::move(connection_lease)) {}

        ~PendingPublicConnection() noexcept { close(); }

        void start(const bool try_immediately) {
            if ((try_immediately && try_assign()) || finished_) {
                return;
            }
            deadline_timer_.expires_after(server_->options_.worker_wait_timeout);
            auto self = shared_from_this();
            deadline_timer_.async_wait([self](const asio::error_code& error) {
                if (!error) {
                    self->stop();
                }
            });
        }

        [[nodiscard]] const std::string& session_key() const noexcept { return session_key_; }

        [[nodiscard]] bool try_assign() {
            if (finished_) {
                return false;
            }
            if (!server_->running_.load() || !server_->session_registry_.is_current(
                                                 binding_.client_id, binding_.session_generation)) {
                stop();
                return false;
            }
            if (!server_->worker_pool_.assign(binding_, public_socket_, connection_lease_)) {
                return false;
            }
            finish();
            return true;
        }

        void stop() noexcept {
            close();
            finish();
        }

      private:
        void close() noexcept {
            asio::error_code ignored;
            try {
                static_cast<void>(deadline_timer_.cancel());
            } catch (...) {
            }
            public_socket_.close(ignored);
        }

        void finish() noexcept {
            if (finished_) {
                return;
            }
            finished_ = true;
            try {
                static_cast<void>(deadline_timer_.cancel());
            } catch (...) {
            }
            server_->pending_connection_finished(this);
        }

        std::shared_ptr<Impl> server_;
        TunnelBinding binding_;
        std::string session_key_;
        asio::ip::tcp::socket public_socket_;
        asio::steady_timer deadline_timer_;
        ConnectionQuota::Lease connection_lease_;
        bool finished_{false};
    };

    Impl(asio::io_context& io_context, ServerOptions options,
         const asio::ip::tcp::endpoint& listen_endpoint,
         std::shared_ptr<asio::ssl::context> tls_context,
         std::shared_ptr<ClientPolicyStore> client_policies, std::string server_id,
         common::PortRange allowed_ports)
        : io_context_(io_context), options_(std::move(options)),
          strand_(asio::make_strand(io_context)), acceptor_(strand_), accept_retry_timer_(strand_),
          worker_request_retry_timer_(strand_), shutdown_timer_(strand_),
          listen_endpoint_(listen_endpoint), tls_context_(std::move(tls_context)),
          client_policies_(std::move(client_policies)), server_id_(std::move(server_id)),
          session_registry_(options_.max_clients),
          worker_pool_(options_.max_idle_workers, options_.max_total_idle_workers),
          connection_quota_(options_.max_connections_per_client, options_.max_total_connections),
          tunnel_registry_(
              strand_, io_context_.get_executor(), allowed_ports, options_.max_tunnels_per_client,
              options_.max_total_tunnels,
              [this](TunnelBinding binding, asio::ip::tcp::socket public_socket) mutable {
                  handle_public_connection(std::move(binding), std::move(public_socket));
              },
              options_.max_udp_peer_sessions) {}

    void handle_public_connection(TunnelBinding binding, asio::ip::tcp::socket public_socket) {
        if (!running_.load()) {
            asio::error_code ignored;
            public_socket.close(ignored);
            return;
        }
        const auto policy = client_policies_->find(binding.client_id);
        if (policy == nullptr || !policy->enabled) {
            asio::error_code ignored;
            public_socket.close(ignored);
            return;
        }
        asio::error_code endpoint_error;
        const auto source_address = public_socket.remote_endpoint(endpoint_error).address();
        if (endpoint_error ||
            !admits_source(*policy, source_limiter_, binding.client_id, source_address,
                           std::chrono::steady_clock::now())) {
            source_rejections_total_.fetch_add(1U, std::memory_order_relaxed);
            asio::error_code ignored;
            public_socket.close(ignored);
            if (!endpoint_error) {
                common::log_warn("public connection rejected by source policy",
                                 {.component = "server.audit",
                                  .server_id = server_id_,
                                  .tunnel_id = binding.tunnel_id,
                                  .error_code = common::ErrorCode::permission_denied});
            }
            return;
        }
        auto connection_lease =
            connection_quota_.try_acquire(binding.client_id, policy->max_connections);
        if (!connection_lease) {
            quota_rejections_total_.fetch_add(1U, std::memory_order_relaxed);
            asio::error_code ignored;
            public_socket.close(ignored);
            common::log_warn("public connection quota rejected a relay",
                             {.component = "server.audit",
                              .server_id = server_id_,
                              .tunnel_id = binding.tunnel_id,
                              .error_code = connection_lease.error().code()});
            return;
        }
        const std::string pending_client_id = binding.client_id;
        const std::uint64_t pending_generation = binding.session_generation;
        auto pending = std::make_shared<PendingPublicConnection>(
            shared_from_this(), std::move(binding), std::move(public_socket),
            std::move(*connection_lease));
        pending_connections_.emplace(pending.get(), pending);
        auto& queue = pending_connections_by_session_[pending->session_key()];
        const bool first_waiter = queue.empty();
        queue.push_back(pending.get());
        pending_connection_positions_.emplace(pending.get(), std::prev(queue.end()));
        refresh_resource_gauges();
        pending->start(first_waiter);
        maybe_request_workers_for_pending(pending_client_id, pending_generation);
        schedule_worker_request_retry();
    }

    void pending_connection_finished(PendingPublicConnection* pending) noexcept {
        const auto group = pending_connections_by_session_.find(pending->session_key());
        const auto position = pending_connection_positions_.find(pending);
        if (group != pending_connections_by_session_.end() &&
            position != pending_connection_positions_.end()) {
            group->second.erase(position->second);
            pending_connection_positions_.erase(position);
            if (group->second.empty()) {
                pending_connections_by_session_.erase(group);
            }
        }
        pending_connections_.erase(pending);
        refresh_resource_gauges();
    }

    void cancel_pending_for_session(const std::string_view client_id,
                                    const std::uint64_t session_generation) noexcept {
        const std::string key = pending_session_key(client_id, session_generation);
        for (;;) {
            const auto group = pending_connections_by_session_.find(key);
            if (group == pending_connections_by_session_.end() || group->second.empty()) {
                return;
            }
            const auto candidate = group->second.front();
            const auto owned = pending_connections_.find(candidate);
            if (owned == pending_connections_.end()) {
                group->second.pop_front();
                pending_connection_positions_.erase(candidate);
                if (group->second.empty()) {
                    pending_connections_by_session_.erase(group);
                }
                continue;
            }
            auto pending = owned->second;
            pending->stop();
        }
    }

    void notify_worker_available(std::string client_id, const std::uint64_t session_generation) {
        auto self = shared_from_this();
        asio::post(strand_, [self, client_id = std::move(client_id), session_generation] {
            const std::string session_key = pending_session_key(client_id, session_generation);
            for (;;) {
                auto group = self->pending_connections_by_session_.find(session_key);
                if (group == self->pending_connections_by_session_.end() || group->second.empty()) {
                    break;
                }
                PendingPublicConnection* candidate = group->second.front();
                const auto owned = self->pending_connections_.find(candidate);
                if (owned == self->pending_connections_.end()) {
                    group->second.pop_front();
                    self->pending_connection_positions_.erase(candidate);
                    if (group->second.empty()) {
                        self->pending_connections_by_session_.erase(group);
                    }
                    continue;
                }
                auto pending = owned->second;
                if (pending->try_assign()) {
                    break;
                }
                // A failed assignment either removed an obsolete pending connection
                // or found that another notification already consumed the worker.
                // Continue only in the former case.
                if (self->pending_connections_.contains(candidate)) {
                    break;
                }
            }
            self->maybe_request_workers_for_pending(client_id, session_generation);
        });
    }

    void maybe_request_workers_for_pending(const std::string_view client_id,
                                           const std::uint64_t session_generation) {
        const std::string session_key = pending_session_key(client_id, session_generation);
        const auto group = pending_connections_by_session_.find(session_key);
        if (group == pending_connections_by_session_.end() || group->second.empty()) {
            return;
        }
        const auto requested = reserve_worker_request_on_control_strand(
            client_id, session_generation, group->second.size());
        if (!requested.has_value()) {
            return;
        }
        for (const auto& [key, session] : sessions_) {
            static_cast<void>(key);
            if (session->is_control_session(client_id, session_generation)) {
                session->enqueue_worker_request(*requested);
                return;
            }
        }
    }

    void schedule_worker_request_retry() {
        if (worker_request_retry_scheduled_ || pending_connections_by_session_.empty() ||
            shutting_down_) {
            return;
        }
        worker_request_retry_scheduled_ = true;
        worker_request_retry_timer_.expires_after(kWorkerRequestCooldown);
        auto self = shared_from_this();
        worker_request_retry_timer_.async_wait([self](const asio::error_code& error) {
            self->worker_request_retry_scheduled_ = false;
            if (error || self->shutting_down_) {
                return;
            }
            std::vector<std::pair<std::string, std::uint64_t>> sessions;
            sessions.reserve(self->pending_connections_by_session_.size());
            for (const auto& [key, queue] : self->pending_connections_by_session_) {
                if (queue.empty()) {
                    continue;
                }
                const auto separator = key.rfind('/');
                if (separator == std::string::npos || separator + 1U >= key.size()) {
                    continue;
                }
                try {
                    sessions.emplace_back(key.substr(0U, separator),
                                          std::stoull(key.substr(separator + 1U)));
                } catch (...) {
                }
            }
            for (const auto& [client_id, generation] : sessions) {
                self->maybe_request_workers_for_pending(client_id, generation);
            }
            self->schedule_worker_request_retry();
        });
    }

    [[nodiscard]] static std::string pending_session_key(const std::string_view client_id,
                                                         const std::uint64_t session_generation) {
        std::string key{client_id};
        key.push_back('/');
        key.append(std::to_string(session_generation));
        return key;
    }

    void session_finished(std::shared_ptr<ControlSession> session) noexcept {
        auto self = shared_from_this();
        asio::post(strand_, [self, session = std::move(session)]() mutable {
            const auto owned = self->sessions_.find(session.get());
            if (owned != self->sessions_.end()) {
                session->cleanup_on_control_strand();
                self->sessions_.erase(owned);
                if (self->shutting_down_ && self->sessions_.empty()) {
                    try {
                        static_cast<void>(self->shutdown_timer_.cancel());
                    } catch (...) {
                    }
                }
            }

            // The TLS stream and its timers belong to the session strand. Keep
            // one owner alive until that strand runs again so their final
            // destruction cannot race a completion handler on another worker.
            auto session_executor = session->executor();
            asio::post(session_executor, [session = std::move(session)] {});
        });
    }

    void begin_shutdown() noexcept {
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        asio::error_code ignored;
        try {
            static_cast<void>(accept_retry_timer_.cancel());
            static_cast<void>(worker_request_retry_timer_.cancel());
        } catch (...) {
        }
        worker_request_retry_scheduled_ = false;
        acceptor_.cancel(ignored);
        acceptor_.close(ignored);
        reserved_descriptor_.close();
        tunnel_registry_.clear();

        auto pending = std::move(pending_connections_);
        pending_connections_.clear();
        for (auto& [key, connection] : pending) {
            static_cast<void>(key);
            connection->stop();
        }

        worker_pool_.clear();
        refresh_resource_gauges();
        for (auto& [key, session] : sessions_) {
            static_cast<void>(key);
            if (!session->relay_active()) {
                session->request_stop(true);
            }
        }
        if (sessions_.empty()) {
            return;
        }

        shutdown_timer_.expires_after(options_.graceful_shutdown_timeout);
        auto self = shared_from_this();
        shutdown_timer_.async_wait([self](const asio::error_code& error) {
            if (!error) {
                self->force_shutdown();
            }
        });
    }

    void force_shutdown() noexcept {
        for (auto& [key, session] : sessions_) {
            static_cast<void>(key);
            session->force_stop();
        }
    }

    void warn_file_descriptor_budget() const noexcept {
        try {
            struct rlimit limits{};
            if (::getrlimit(RLIMIT_NOFILE, &limits) != 0 || limits.rlim_cur == RLIM_INFINITY) {
                return;
            }
            std::uintmax_t required = 32U;
            const auto add_saturating = [&required](const std::size_t value) {
                const auto converted = static_cast<std::uintmax_t>(value);
                if (converted > std::numeric_limits<std::uintmax_t>::max() - required) {
                    required = std::numeric_limits<std::uintmax_t>::max();
                } else {
                    required += converted;
                }
            };
            add_saturating(options_.max_clients);
            add_saturating(options_.max_total_idle_workers);
            add_saturating(options_.max_total_connections);
            add_saturating(options_.max_total_tunnels);
            if (required <= static_cast<std::uintmax_t>(limits.rlim_cur)) {
                return;
            }
            common::log_warn(
                "configured server resource limits may exceed RLIMIT_NOFILE (required " +
                    std::to_string(required) + ", soft limit " +
                    std::to_string(static_cast<std::uintmax_t>(limits.rlim_cur)) + ")",
                {.component = "server.listener",
                 .server_id = server_id_,
                 .error_code = common::ErrorCode::resource_exhausted});
        } catch (...) {
        }
    }

    void accept_next() {
        if (!running_.load() || !acceptor_.is_open()) {
            return;
        }
        auto self = shared_from_this();
        acceptor_.async_accept(
            asio::make_strand(io_context_),
            asio::bind_executor(strand_, [self](const asio::error_code& error,
                                                asio::ip::tcp::socket socket) mutable {
                if (!error && self->running_.load()) {
                    protocol::configure_tcp_transport(socket);
                    self->accept_retry_policy_.reset();
                    const std::size_t previous = self->active_connections_.fetch_add(1U);
                    self->connections_total_.fetch_add(1U, std::memory_order_relaxed);
                    const std::size_t connection_limit =
                        std::min(kMaxServerConnections, self->options_.max_clients +
                                                            self->options_.max_total_idle_workers +
                                                            self->options_.max_total_connections);
                    if (previous < connection_limit) {
                        auto session = std::make_shared<ControlSession>(std::move(socket), self);
                        self->sessions_.emplace(session.get(), session);
                        session->start();
                    } else {
                        self->active_connections_.fetch_sub(1U);
                        self->quota_rejections_total_.fetch_add(1U, std::memory_order_relaxed);
                        asio::error_code ignored;
                        socket.close(ignored);
                    }
                } else if (AcceptRetryPolicy::retryable(error) && self->running_.load()) {
                    self->handle_accept_failure(error);
                    return;
                }
                if (self->running_.load()) {
                    self->accept_next();
                }
            }));
    }

    void handle_accept_failure(const asio::error_code& error) {
        errors_total_.fetch_add(1U, std::memory_order_relaxed);
        if (AcceptRetryPolicy::descriptor_exhausted(error)) {
            reserved_descriptor_.recover(acceptor_);
        }
        if (accept_retry_policy_.should_log(AcceptRetryPolicy::Clock::now())) {
            common::log_warn(
                "TLS listener accept failed; retrying with backoff",
                {.component = "server.listener",
                 .server_id = server_id_,
                 .error_code =
                     AcceptRetryPolicy::resource_exhausted(error)
                         ? std::optional<common::ErrorCode>{common::ErrorCode::resource_exhausted}
                         : std::optional<common::ErrorCode>{common::ErrorCode::connection_failed}});
        }
        accept_retry_timer_.expires_after(accept_retry_policy_.next_delay());
        auto self = shared_from_this();
        accept_retry_timer_.async_wait([self](const asio::error_code& timer_error) {
            if (!timer_error && self->running_.load() && self->acceptor_.is_open()) {
                self->accept_next();
            }
        });
    }

    asio::io_context& io_context_;
    ServerOptions options_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer accept_retry_timer_;
    asio::steady_timer worker_request_retry_timer_;
    asio::steady_timer shutdown_timer_;
    asio::ip::tcp::endpoint listen_endpoint_;
    std::shared_ptr<asio::ssl::context> tls_context_;
    std::shared_ptr<ClientPolicyStore> client_policies_;
    common::SecureString rejection_psk_{"minitun-rejected-client-policy"};
    std::string server_id_;
    protocol::NonceReplayCache nonce_cache_;
    protocol::AuthRateLimiter auth_rate_limiter_;
    SessionRegistry session_registry_;
    WorkerPool worker_pool_;
    ConnectionQuota connection_quota_;
    TunnelRegistry tunnel_registry_;
    std::unordered_map<ControlSession*, std::shared_ptr<ControlSession>> sessions_;
    std::unordered_map<PendingPublicConnection*, std::shared_ptr<PendingPublicConnection>>
        pending_connections_;
    using PendingConnectionQueue = std::list<PendingPublicConnection*>;
    std::unordered_map<std::string, PendingConnectionQueue> pending_connections_by_session_;
    std::unordered_map<PendingPublicConnection*, PendingConnectionQueue::iterator>
        pending_connection_positions_;
    std::unordered_map<std::string, WorkerRequestState> worker_request_states_;
    std::atomic<std::size_t> active_connections_{0U};
    std::atomic<std::uint64_t> active_tunnels_{0U};
    std::atomic<std::uint64_t> idle_workers_{0U};
    std::atomic<std::uint64_t> active_relays_{0U};
    std::atomic<std::uint64_t> pending_connection_count_{0U};
    std::atomic<std::uint64_t> connections_total_{0U};
    std::atomic<std::uint64_t> tls_resumptions_total_{0U};
    std::atomic<std::uint64_t> authentication_success_total_{0U};
    std::atomic<std::uint64_t> authentication_failure_total_{0U};
    std::atomic<std::uint64_t> registration_success_total_{0U};
    std::atomic<std::uint64_t> registration_failure_total_{0U};
    std::atomic<std::uint64_t> unregistration_total_{0U};
    std::atomic<std::uint64_t> relay_total_{0U};
    std::atomic<std::uint64_t> relay_bytes_in_total_{0U};
    std::atomic<std::uint64_t> relay_bytes_out_total_{0U};
    std::atomic<std::uint64_t> acl_rejections_total_{0U};
    std::atomic<std::uint64_t> quota_rejections_total_{0U};
    std::atomic<std::uint64_t> source_rejections_total_{0U};
    SourceConnectionLimiter source_limiter_;
    std::atomic<std::uint64_t> errors_total_{0U};
    std::atomic<std::uint64_t> policy_reloads_total_{0U};
    std::atomic<std::uint64_t> policy_reload_failures_total_{0U};
    std::atomic<std::uint64_t> registration_latency_microseconds_total_{0U};
    std::atomic<std::uint16_t> listening_port_{0U};
    std::atomic<bool> running_{false};
    ReservedFileDescriptor reserved_descriptor_;
    AcceptRetryPolicy accept_retry_policy_;
    bool worker_request_retry_scheduled_{false};
    bool shutting_down_{false};

    void refresh_resource_gauges() noexcept {
        active_tunnels_.store(tunnel_registry_.size(), std::memory_order_relaxed);
        idle_workers_.store(worker_pool_.size(), std::memory_order_relaxed);
        pending_connection_count_.store(pending_connections_.size(), std::memory_order_relaxed);
    }
};

common::Result<std::unique_ptr<Server>> Server::create(asio::io_context& io_context,
                                                       ServerOptions options) {
    auto implementation = Impl::create(io_context, std::move(options));
    if (!implementation) {
        return common::Result<std::unique_ptr<Server>>::failure(implementation.error());
    }
    return std::unique_ptr<Server>{new Server{std::move(*implementation)}};
}

Server::Server(std::shared_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

Server::~Server() noexcept { stop(); }

common::Result<void> Server::start() { return implementation_->start(); }

common::Result<void> Server::reload() { return implementation_->reload(); }

void Server::stop() noexcept { implementation_->stop(); }

std::uint16_t Server::listening_port() const noexcept { return implementation_->listening_port(); }

const std::string& Server::server_id() const noexcept { return implementation_->server_id(); }

ServerMetrics Server::metrics() const noexcept { return implementation_->metrics(); }

} // namespace minitun::server
