#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/daemon/worker_pool.hpp>
#include <minitun/server/worker_pool.hpp>

namespace minitun::server {
namespace {

[[nodiscard]] std::string generated_id(const common::IdKind kind) {
    auto id = common::Id::generate(kind);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

[[nodiscard]] TunnelBinding binding_for(const std::string& client_id,
                                        const std::uint64_t generation) {
    return {
        .client_id = client_id,
        .session_generation = generation,
        .tunnel_id = generated_id(common::IdKind::tunnel),
        .bind_host = "127.0.0.1",
        .bind_port = 6'000U,
    };
}

[[nodiscard]] daemon::WorkerPoolOptions valid_daemon_options() {
    auto endpoint = common::Endpoint::parse("127.0.0.1:2333");
    if (!endpoint) {
        throw std::runtime_error("deterministic worker endpoint is invalid");
    }
    return {
        .endpoint = std::move(*endpoint),
        .server_id = generated_id(common::IdKind::server),
        .remote_server_id = generated_id(common::IdKind::server),
        .client_id = generated_id(common::IdKind::client),
        .psk = std::make_shared<const common::SecureString>("secret"),
        .session_generation = 1U,
        .min_idle_workers = 0U,
        .max_idle_workers = 0U,
    };
}

TEST(WorkerPoolTest, AssignsOnlyMatchingClientGeneration) {
    asio::io_context io_context;
    WorkerPool pool{4U, 8U};
    ConnectionQuota connection_quota{4U, 8U};
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string worker_id = generated_id(common::IdKind::connection);
    bool assigned = false;

    ASSERT_TRUE(pool.add({client_id, 7U, worker_id},
                         [&assigned](TunnelBinding, asio::ip::tcp::socket, ConnectionQuota::Lease) {
                             assigned = true;
                         }));
    asio::ip::tcp::socket public_socket{io_context};
    public_socket.open(asio::ip::tcp::v4());
    auto connection_lease = connection_quota.try_acquire(client_id);
    ASSERT_TRUE(connection_lease);
    EXPECT_FALSE(pool.assign(binding_for(client_id, 8U), public_socket, *connection_lease));
    EXPECT_TRUE(public_socket.is_open());
    EXPECT_EQ(pool.size(), 1U);

    EXPECT_TRUE(pool.assign(binding_for(client_id, 7U), public_socket, *connection_lease));
    EXPECT_TRUE(assigned);
    EXPECT_EQ(pool.size(), 0U);
}

TEST(WorkerPoolTest, EnforcesPerSessionAndGlobalIdleLimits) {
    WorkerPool pool{1U, 2U};
    const std::string first_client = generated_id(common::IdKind::client);
    const std::string second_client = generated_id(common::IdKind::client);
    const auto handler = [](TunnelBinding, asio::ip::tcp::socket, ConnectionQuota::Lease) {};

    ASSERT_TRUE(pool.add({first_client, 1U, generated_id(common::IdKind::connection)}, handler));
    const auto per_session =
        pool.add({first_client, 1U, generated_id(common::IdKind::connection)}, handler);
    ASSERT_FALSE(per_session);
    EXPECT_EQ(per_session.error().code(), common::ErrorCode::resource_exhausted);

    ASSERT_TRUE(pool.add({second_client, 1U, generated_id(common::IdKind::connection)}, handler));
    const auto global =
        pool.add({second_client, 2U, generated_id(common::IdKind::connection)}, handler);
    ASSERT_FALSE(global);
    EXPECT_EQ(global.error().code(), common::ErrorCode::resource_exhausted);
}

TEST(WorkerPoolTest, RemovesWorkersBySessionAndClient) {
    WorkerPool pool{4U, 8U};
    const std::string client_id = generated_id(common::IdKind::client);
    const auto handler = [](TunnelBinding, asio::ip::tcp::socket, ConnectionQuota::Lease) {};
    std::size_t removals = 0U;
    ASSERT_TRUE(pool.add({client_id, 1U, generated_id(common::IdKind::connection)}, handler,
                         [&removals] { ++removals; }));
    ASSERT_TRUE(pool.add({client_id, 2U, generated_id(common::IdKind::connection)}, handler));

    pool.remove_session(client_id, 1U);
    EXPECT_EQ(removals, 1U);
    EXPECT_EQ(pool.idle_count(client_id, 1U), 0U);
    EXPECT_EQ(pool.idle_count(client_id, 2U), 1U);
    pool.remove_client(client_id);
    EXPECT_EQ(pool.size(), 0U);
}

TEST(WorkerPoolTest, RejectsEveryInvalidRegistrationAndContainsCallbacks) {
    asio::io_context io_context;
    WorkerPool pool{2U, 4U};
    ConnectionQuota quota{2U, 4U};
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string worker_id = generated_id(common::IdKind::connection);
    const auto handler = [](TunnelBinding, asio::ip::tcp::socket, ConnectionQuota::Lease) {};

    EXPECT_FALSE(pool.add({generated_id(common::IdKind::server), 1U, worker_id}, handler));
    EXPECT_FALSE(pool.add({client_id, 1U, generated_id(common::IdKind::server)}, handler));
    EXPECT_FALSE(pool.add({client_id, 0U, worker_id}, handler));
    EXPECT_FALSE(pool.add({client_id, 1U, worker_id}, {}));
    ASSERT_TRUE(pool.add({client_id, 1U, worker_id}, handler,
                         [] { throw std::runtime_error("contained removal failure"); }));
    const auto duplicate = pool.add({client_id, 1U, worker_id}, handler);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::already_exists);
    pool.remove("missing-worker");
    pool.remove(worker_id);
    EXPECT_EQ(pool.size(), 0U);

    const std::string throwing_worker = generated_id(common::IdKind::connection);
    ASSERT_TRUE(pool.add({client_id, 1U, throwing_worker, 1U},
                         [](TunnelBinding, asio::ip::tcp::socket, ConnectionQuota::Lease) {
                             throw std::runtime_error("contained assignment failure");
                         }));
    asio::ip::tcp::socket public_socket{io_context};
    public_socket.open(asio::ip::tcp::v4());
    auto lease = quota.try_acquire(client_id);
    ASSERT_TRUE(lease);
    EXPECT_TRUE(pool.assign(binding_for(client_id, 1U), public_socket, *lease));
    EXPECT_FALSE(public_socket.is_open());
}

TEST(WorkerPoolTest, AppliesNegotiatedLimitAndHandlesZeroConfiguredLimits) {
    const auto handler = [](TunnelBinding, asio::ip::tcp::socket, ConnectionQuota::Lease) {};
    const std::string client_id = generated_id(common::IdKind::client);
    WorkerPool negotiated{4U, 8U};
    ASSERT_TRUE(
        negotiated.add({client_id, 1U, generated_id(common::IdKind::connection), 1U}, handler));
    const auto bounded =
        negotiated.add({client_id, 1U, generated_id(common::IdKind::connection), 1U}, handler);
    ASSERT_FALSE(bounded);
    EXPECT_EQ(bounded.error().code(), common::ErrorCode::resource_exhausted);
    negotiated.clear();

    WorkerPool no_session_capacity{0U, 8U};
    EXPECT_FALSE(no_session_capacity.add({client_id, 1U, generated_id(common::IdKind::connection)},
                                         handler));
    WorkerPool no_global_capacity{4U, 0U};
    EXPECT_FALSE(
        no_global_capacity.add({client_id, 1U, generated_id(common::IdKind::connection)}, handler));
}

TEST(WorkerBudgetTest, EnforcesAndReleasesGlobalLimit) {
    daemon::WorkerBudget budget{2U};
    EXPECT_TRUE(budget.try_acquire());
    EXPECT_TRUE(budget.try_acquire());
    EXPECT_FALSE(budget.try_acquire());
    EXPECT_EQ(budget.in_use(), 2U);
    budget.release();
    EXPECT_EQ(budget.in_use(), 1U);
    EXPECT_TRUE(budget.try_acquire());
    EXPECT_EQ(budget.maximum(), 2U);

    daemon::WorkerBudget normalized{0U};
    EXPECT_EQ(normalized.maximum(), 1U);
    normalized.release();
    EXPECT_EQ(normalized.in_use(), 0U);
}

TEST(DaemonWorkerPoolTest, AcceptsNegotiatedIdleTimeoutIncludingMaximumGrace) {
    asio::io_context io_context;
    auto endpoint = common::Endpoint::parse("127.0.0.1:2333");
    ASSERT_TRUE(endpoint) << endpoint.error();
    auto tls_context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
    auto idle_budget = std::make_shared<daemon::WorkerBudget>(4U);
    auto connection_budget = std::make_shared<daemon::WorkerBudget>(8U);
    auto pool = daemon::WorkerPool::create(
        io_context.get_executor(), std::move(tls_context), std::move(idle_budget),
        std::move(connection_budget),
        daemon::WorkerPoolOptions{
            .endpoint = std::move(*endpoint),
            .server_id = generated_id(common::IdKind::server),
            .remote_server_id = generated_id(common::IdKind::server),
            .client_id = generated_id(common::IdKind::client),
            .psk = std::make_shared<const common::SecureString>("secret"),
            .session_generation = 1U,
            .min_idle_workers = 0U,
            .max_idle_workers = 0U,
        },
        [](const std::string_view) {
            return common::Result<common::Endpoint>::failure(common::ErrorCode::not_found,
                                                             "unused test resolver");
        });
    ASSERT_TRUE(pool) << pool.error();
    EXPECT_TRUE((*pool)->set_idle_timeout(std::chrono::seconds{305}));
    EXPECT_FALSE((*pool)->set_idle_timeout(std::chrono::seconds{306}));
    EXPECT_FALSE((*pool)->set_idle_timeout(std::chrono::seconds{0}));
}

TEST(DaemonWorkerPoolTest, RejectsEveryIndependentOptionAndMissingDependency) {
    asio::io_context io_context;
    const auto resolver = [](const std::string_view) {
        return common::Result<common::Endpoint>::failure(common::ErrorCode::not_found, "unused");
    };
    const auto expect_invalid = [&io_context, &resolver](daemon::WorkerPoolOptions options) {
        const auto result = daemon::WorkerPool::create(
            io_context.get_executor(),
            std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client),
            std::make_shared<daemon::WorkerBudget>(4U), std::make_shared<daemon::WorkerBudget>(8U),
            std::move(options), resolver);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument) << result.error();
    };

    {
        auto options = valid_daemon_options();
        options.server_id = generated_id(common::IdKind::client);
        expect_invalid(std::move(options));
    }
    {
        auto options = valid_daemon_options();
        options.remote_server_id = generated_id(common::IdKind::client);
        expect_invalid(std::move(options));
    }
    {
        auto options = valid_daemon_options();
        options.client_id = generated_id(common::IdKind::server);
        expect_invalid(std::move(options));
    }
    {
        auto options = valid_daemon_options();
        options.psk.reset();
        expect_invalid(std::move(options));
    }
    {
        auto options = valid_daemon_options();
        options.psk = std::make_shared<const common::SecureString>("");
        expect_invalid(std::move(options));
    }
    {
        auto options = valid_daemon_options();
        options.session_generation = 0U;
        expect_invalid(std::move(options));
    }
    {
        auto options = valid_daemon_options();
        options.min_idle_workers = 2U;
        options.max_idle_workers = 1U;
        expect_invalid(std::move(options));
    }
    {
        auto options = valid_daemon_options();
        options.max_idle_workers = 129U;
        expect_invalid(std::move(options));
    }

    const auto expect_bad_timeout = [&expect_invalid](auto member,
                                                      const std::chrono::seconds timeout) {
        auto options = valid_daemon_options();
        options.*member = timeout;
        expect_invalid(std::move(options));
    };
    expect_bad_timeout(&daemon::WorkerPoolOptions::connect_timeout, std::chrono::seconds::zero());
    expect_bad_timeout(&daemon::WorkerPoolOptions::connect_timeout, std::chrono::seconds{301});
    expect_bad_timeout(&daemon::WorkerPoolOptions::handshake_timeout, std::chrono::seconds::zero());
    expect_bad_timeout(&daemon::WorkerPoolOptions::handshake_timeout, std::chrono::seconds{301});
    expect_bad_timeout(&daemon::WorkerPoolOptions::idle_timeout, std::chrono::seconds::zero());
    expect_bad_timeout(&daemon::WorkerPoolOptions::idle_timeout, std::chrono::seconds{306});
    expect_bad_timeout(&daemon::WorkerPoolOptions::relay_inactivity_timeout,
                       std::chrono::seconds::zero());
    expect_bad_timeout(&daemon::WorkerPoolOptions::relay_inactivity_timeout,
                       std::chrono::hours{24} + std::chrono::seconds{1});
    expect_bad_timeout(&daemon::WorkerPoolOptions::graceful_shutdown_timeout,
                       std::chrono::seconds::zero());
    expect_bad_timeout(&daemon::WorkerPoolOptions::graceful_shutdown_timeout,
                       std::chrono::seconds{301});

    auto valid = valid_daemon_options();
    auto tls_context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
    auto idle_budget = std::make_shared<daemon::WorkerBudget>(4U);
    auto connection_budget = std::make_shared<daemon::WorkerBudget>(8U);
    EXPECT_FALSE(daemon::WorkerPool::create(io_context.get_executor(), nullptr, idle_budget,
                                            connection_budget, valid, resolver));
    EXPECT_FALSE(daemon::WorkerPool::create(io_context.get_executor(), tls_context, nullptr,
                                            connection_budget, valid, resolver));
    EXPECT_FALSE(daemon::WorkerPool::create(io_context.get_executor(), tls_context, idle_budget,
                                            nullptr, valid, resolver));
    EXPECT_FALSE(daemon::WorkerPool::create(io_context.get_executor(), tls_context, idle_budget,
                                            connection_budget, valid, {}));
}

TEST(DaemonWorkerPoolTest, LifecycleIsIdempotentWithoutRequestedIdleWorkers) {
    asio::io_context io_context;
    auto pool = daemon::WorkerPool::create(
        io_context.get_executor(),
        std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client),
        std::make_shared<daemon::WorkerBudget>(4U), std::make_shared<daemon::WorkerBudget>(8U),
        valid_daemon_options(), [](const std::string_view) {
            return common::Result<common::Endpoint>::failure(common::ErrorCode::not_found,
                                                             "unused");
        });
    ASSERT_TRUE(pool) << pool.error();
    EXPECT_EQ((*pool)->size(), 0U);
    ASSERT_TRUE((*pool)->start());
    const auto duplicate = (*pool)->start();
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::already_exists);
    (*pool)->request_workers(0U);
    (*pool)->request_workers(1U);
    io_context.poll();
    EXPECT_EQ((*pool)->size(), 0U);
    (*pool)->stop();
    (*pool)->stop();
    EXPECT_TRUE((*pool)->start());
    (*pool)->stop();
    io_context.restart();
    io_context.poll();
}

} // namespace
} // namespace minitun::server
