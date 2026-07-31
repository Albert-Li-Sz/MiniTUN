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
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/protocol/messages.hpp>
#include <minitun/protocol/relay.hpp>
#include <minitun/protocol/state_machine.hpp>
#include <minitun/protocol/tls.hpp>

namespace minitun::daemon {
namespace {

inline constexpr std::chrono::seconds kMaximumWorkerTimeout{300};
inline constexpr std::chrono::hours kMaximumRelayTimeout{24};

[[nodiscard]] common::Result<void> validate_options(const WorkerPoolOptions& options) {
    if (!common::Id::parse(options.server_id, common::IdKind::server) ||
        !common::Id::parse(options.client_id, common::IdKind::client) ||
        options.session_generation == 0U) {
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
        options.idle_timeout > kMaximumWorkerTimeout ||
        options.relay_inactivity_timeout <= std::chrono::seconds::zero() ||
        options.relay_inactivity_timeout > kMaximumRelayTimeout) {
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
        WorkerSession(std::shared_ptr<Impl> pool, std::string worker_id)
            : pool_(pool), worker_id_(std::move(worker_id)), server_id_(pool->options_.server_id),
              remote_endpoint_(pool->options_.endpoint.to_string()), resolver_(pool->executor_),
              stream_(pool->executor_, *pool->tls_context_), operation_timer_(pool->executor_),
              state_(protocol::PeerRole::client, protocol::ConnectionKind::worker) {}

        ~WorkerSession() noexcept { close(); }

        void start() {
            auto self = shared_from_this();
            asio::co_spawn(stream_.get_executor(), run(), [self](const std::exception_ptr failure) {
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

            auto configured = protocol::configure_client_tls_stream(
                stream_, pool->options_.endpoint.host(), pool->options_.insecure_skip_verify);
            if (!configured) {
                co_return;
            }
            arm_timeout(pool->options_.handshake_timeout);
            co_await stream_.async_handshake(asio::ssl::stream_base::client,
                                             asio::redirect_error(asio::use_awaitable, error));
            cancel_timeout();
            if (error) {
                co_return;
            }

            auto hello_payload = protocol::encode_worker_hello({
                .client_id = pool->options_.client_id,
                .session_generation = pool->options_.session_generation,
                .worker_id = worker_id_,
            });
            if (!hello_payload || !co_await write_frame({protocol::MessageType::worker_hello, 0U,
                                                         1U, std::move(*hello_payload)},
                                                        pool->options_.handshake_timeout)) {
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

            auto relay_frame = co_await read_frame(pool->options_.idle_timeout);
            if (!relay_frame || relay_frame->type != protocol::MessageType::start_relay) {
                co_return;
            }
            auto relay = protocol::decode_start_relay(relay_frame->payload);
            if (!relay) {
                co_return;
            }
            pool->worker_consumed(worker_id_);

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

            auto connected_payload = protocol::encode_local_connect_ok({relay->connection_id});
            if (!connected_payload ||
                !co_await write_frame({protocol::MessageType::local_connect_ok, 0U,
                                       relay_frame->request_id, std::move(*connected_payload)},
                                      pool->options_.handshake_timeout)) {
                co_return;
            }

            auto relayed = co_await protocol::relay_tls_and_tcp(
                stream_, *local_socket_,
                {.inactivity_timeout = pool->options_.relay_inactivity_timeout});
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
                static_cast<void>(co_await write_frame({protocol::MessageType::local_connect_error,
                                                        0U, request_id, std::move(*failed_payload)},
                                                       timeout));
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

        [[nodiscard]] asio::awaitable<bool> write_frame(protocol::Frame frame,
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
            cancel_timeout();
            if (local_socket_ != nullptr) {
                asio::error_code ignored;
                local_socket_->cancel(ignored);
                local_socket_->close(ignored);
                local_socket_.reset();
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
        protocol::TlsStream stream_;
        std::unique_ptr<asio::ip::tcp::socket> local_socket_;
        asio::steady_timer operation_timer_;
        protocol::StateMachine state_;
    };

  public:
    [[nodiscard]] static common::Result<std::shared_ptr<Impl>>
    create(asio::any_io_executor executor, std::shared_ptr<asio::ssl::context> tls_context,
           std::shared_ptr<WorkerBudget> budget, WorkerPoolOptions options,
           LocalEndpointResolver local_endpoint_resolver) {
        auto valid = validate_options(options);
        if (!valid) {
            return common::Result<std::shared_ptr<Impl>>::failure(valid.error());
        }
        if (!tls_context || !budget || !local_endpoint_resolver) {
            return common::Result<std::shared_ptr<Impl>>::failure(
                common::ErrorCode::invalid_argument, "worker pool dependency is unavailable");
        }
        return std::shared_ptr<Impl>{new Impl(std::move(executor), std::move(tls_context),
                                              std::move(budget), std::move(options),
                                              std::move(local_endpoint_resolver))};
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
            auto sessions = std::move(self->sessions_);
            const std::size_t budget_slots = self->available_workers_.size();
            self->available_workers_.clear();
            self->size_.store(0U);
            for (auto& [worker_id, session] : sessions) {
                static_cast<void>(worker_id);
                session->stop();
            }
            for (std::size_t index = 0U; index < budget_slots; ++index) {
                self->budget_->release();
            }
        });
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_.load(); }

  private:
    Impl(asio::any_io_executor executor, std::shared_ptr<asio::ssl::context> tls_context,
         std::shared_ptr<WorkerBudget> budget, WorkerPoolOptions options,
         LocalEndpointResolver local_endpoint_resolver)
        : executor_(std::move(executor)), tls_context_(std::move(tls_context)),
          budget_(std::move(budget)), options_(std::move(options)),
          local_endpoint_resolver_(std::move(local_endpoint_resolver)),
          replenish_timer_(executor_) {}

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
            if (!budget_->try_acquire()) {
                break;
            }
            auto worker_id = common::Id::generate(common::IdKind::connection);
            if (!worker_id) {
                budget_->release();
                break;
            }
            const std::string key = worker_id->str();
            auto session = std::make_shared<WorkerSession>(shared_from_this(), key);
            sessions_.emplace(key, session);
            available_workers_.insert(key);
            size_.store(available_workers_.size());
            session->start();
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
        if (!running_.load() || available_workers_.size() >= options_.min_idle_workers) {
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

    asio::any_io_executor executor_;
    std::shared_ptr<asio::ssl::context> tls_context_;
    std::shared_ptr<WorkerBudget> budget_;
    WorkerPoolOptions options_;
    LocalEndpointResolver local_endpoint_resolver_;
    asio::steady_timer replenish_timer_;
    std::unordered_map<std::string, std::shared_ptr<WorkerSession>> sessions_;
    std::unordered_set<std::string> available_workers_;
    std::atomic<std::size_t> size_{0U};
    std::atomic<bool> running_{false};
};

common::Result<std::unique_ptr<WorkerPool>>
WorkerPool::create(asio::any_io_executor executor, std::shared_ptr<asio::ssl::context> tls_context,
                   std::shared_ptr<WorkerBudget> budget, WorkerPoolOptions options,
                   LocalEndpointResolver local_endpoint_resolver) {
    auto implementation =
        Impl::create(std::move(executor), std::move(tls_context), std::move(budget),
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

void WorkerPool::request_workers(const std::uint16_t count) {
    implementation_->request_workers(count);
}

void WorkerPool::stop() noexcept { implementation_->stop(); }

std::size_t WorkerPool::size() const noexcept { return implementation_->size(); }

} // namespace minitun::daemon
