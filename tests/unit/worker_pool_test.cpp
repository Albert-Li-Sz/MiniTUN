#include <chrono>
#include <cstdint>
#include <memory>
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
}

TEST(DaemonWorkerPoolTest, AcceptsNegotiatedIdleTimeoutIncludingMaximumGrace) {
    asio::io_context io_context;
    auto endpoint = common::Endpoint::parse("127.0.0.1:2333");
    ASSERT_TRUE(endpoint) << endpoint.error();
    auto tls_context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
    auto idle_budget = std::make_shared<daemon::WorkerBudget>(4U);
    auto connection_budget = std::make_shared<daemon::WorkerBudget>(8U);
    auto pool =
        daemon::WorkerPool::create(io_context.get_executor(), std::move(tls_context),
                                   std::move(idle_budget), std::move(connection_budget),
                                   daemon::WorkerPoolOptions{
                                       .endpoint = std::move(*endpoint),
                                       .server_id = generated_id(common::IdKind::server),
                                       .client_id = generated_id(common::IdKind::client),
                                       .session_generation = 1U,
                                       .min_idle_workers = 0U,
                                       .max_idle_workers = 0U,
                                   },
                                   [](const std::string_view) {
                                       return common::Result<common::Endpoint>::failure(
                                           common::ErrorCode::not_found, "unused test resolver");
                                   });
    ASSERT_TRUE(pool) << pool.error();
    EXPECT_TRUE((*pool)->set_idle_timeout(std::chrono::seconds{305}));
    EXPECT_FALSE((*pool)->set_idle_timeout(std::chrono::seconds{306}));
    EXPECT_FALSE((*pool)->set_idle_timeout(std::chrono::seconds{0}));
}

} // namespace
} // namespace minitun::server
