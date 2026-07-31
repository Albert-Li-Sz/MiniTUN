#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/daemon/control_service.hpp>
#include <minitun/ipc/dispatcher.hpp>
#include <minitun/ipc/protocol.hpp>
#include <minitun/storage/credential_store.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

#include "storage_test_support.hpp"

namespace minitun::daemon {
namespace {

using storage::test::TemporaryDatabaseFile;

[[nodiscard]] ipc::Response dispatch(ipc::Dispatcher& dispatcher, std::string method,
                                     ipc::Json params) {
    auto request_id = common::Id::generate(common::IdKind::request);
    if (!request_id) {
        throw std::runtime_error("failed to generate test request ID");
    }
    return dispatcher.dispatch(ipc::Request{
        ipc::kProtocolVersion,
        std::move(*request_id),
        std::move(method),
        std::move(params),
    });
}

[[nodiscard]] const ipc::Json& require_result(const ipc::Response& response) {
    if (!response.ok() || response.result() == nullptr) {
        throw std::runtime_error("expected successful daemon control response");
    }
    return *response.result();
}

class DaemonControlServiceTest : public testing::Test {
  protected:
    void SetUp() override {
        auto opened_repository = storage::StateRepository::open(state_file_.path_string());
        ASSERT_TRUE(opened_repository) << opened_repository.error();
        repository_ = std::move(*opened_repository);

        auto opened_credentials =
            storage::SqliteCredentialStore::open(credential_file_.path_string());
        ASSERT_TRUE(opened_credentials) << opened_credentials.error();
        credentials_ = std::move(*opened_credentials);

        service_ = std::make_unique<ControlService>(*repository_, *credentials_);
        const auto registered = service_->register_handlers(dispatcher_);
        ASSERT_TRUE(registered) << registered.error();
    }

    TemporaryDatabaseFile state_file_;
    TemporaryDatabaseFile credential_file_;
    std::unique_ptr<storage::StateRepository> repository_;
    std::unique_ptr<storage::SqliteCredentialStore> credentials_;
    std::unique_ptr<ControlService> service_;
    ipc::Dispatcher dispatcher_;
};

TEST_F(DaemonControlServiceTest, RegistersCompleteStageFourMethodSet) {
    EXPECT_EQ(dispatcher_.size(), 11U);

    const auto daemon_status = dispatch(dispatcher_, "daemon.status", ipc::Json::object());
    ASSERT_TRUE(daemon_status.ok());
    EXPECT_EQ(require_result(daemon_status).at("state"), "running");

    const auto status = dispatch(dispatcher_, "status", ipc::Json::object());
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(require_result(status).at("servers").at("total"), 0);
    EXPECT_EQ(require_result(status).at("tunnels").at("total"), 0);
}

TEST_F(DaemonControlServiceTest, PersistsServerLoginAndOfflineTunnelWithoutLeakingToken) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"},
                                          {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const auto& added_server = require_result(added).at("server");
    EXPECT_EQ(added_server.at("actual_state"), "not_authenticated");
    EXPECT_FALSE(added_server.at("credential_configured").get<bool>());
    EXPECT_FALSE(added_server.contains("credential_ref"));
    const std::string server_id_text = added_server.at("id").get<std::string>();

    constexpr std::string_view token = "unit-test-private-token";
    const auto logged_in = dispatch(dispatcher_, "server.login",
                                    ipc::Json{{"identifier", "primary"}, {"token", token}});
    ASSERT_TRUE(logged_in.ok()) << *logged_in.error();
    const std::string login_wire = require_result(logged_in).dump();
    EXPECT_EQ(login_wire.find(token), std::string::npos);
    EXPECT_EQ(login_wire.find("credential_ref"), std::string::npos);
    EXPECT_EQ(require_result(logged_in).at("server").at("actual_state"), "disconnected");

    auto server_id = common::Id::parse(server_id_text, common::IdKind::server);
    ASSERT_TRUE(server_id) << server_id.error();
    auto persisted_server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(persisted_server) << persisted_server.error();
    ASSERT_TRUE(persisted_server->credential_ref.has_value());
    auto persisted_token = credentials_->get(*persisted_server->credential_ref);
    ASSERT_TRUE(persisted_token) << persisted_token.error();
    EXPECT_EQ(persisted_token->view(), token);

    const auto tunnel_added = dispatch(
        dispatcher_, "tun.add",
        ipc::Json{{"server", "primary"}, {"local_port", 22}, {"remote_port", 6000},
                  {"name", "ssh"}});
    ASSERT_TRUE(tunnel_added.ok()) << *tunnel_added.error();
    const auto& tunnel = require_result(tunnel_added).at("tunnel");
    EXPECT_EQ(tunnel.at("desired_state"), "active");
    EXPECT_EQ(tunnel.at("actual_state"), "pending");
    EXPECT_EQ(tunnel.at("server_id"), server_id_text);

    const auto status = dispatch(dispatcher_, "status", ipc::Json::object());
    ASSERT_TRUE(status.ok()) << *status.error();
    EXPECT_EQ(require_result(status).at("servers").at("total"), 1);
    EXPECT_EQ(require_result(status).at("tunnels").at("total"), 1);
    EXPECT_EQ(require_result(status).at("tunnels").at("active"), 0);
}

TEST_F(DaemonControlServiceTest, RemovalUsesTombstonesAndFiltersPublicLists) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"},
                                          {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const std::string server_id_text =
        require_result(added).at("server").at("id").get<std::string>();
    auto server_id = common::Id::parse(server_id_text, common::IdKind::server);
    ASSERT_TRUE(server_id) << server_id.error();

    const auto logged_in = dispatch(dispatcher_, "server.login",
                                    ipc::Json{{"identifier", "primary"}, {"token", "token"}});
    ASSERT_TRUE(logged_in.ok()) << *logged_in.error();
    auto server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(server) << server.error();
    ASSERT_TRUE(server->credential_ref.has_value());
    const std::string credential_key = *server->credential_ref;

    const auto tunnel_added = dispatch(
        dispatcher_, "tun.add",
        ipc::Json{{"server", "primary"}, {"local_port", 22}, {"remote_port", 6000},
                  {"name", "ssh"}});
    ASSERT_TRUE(tunnel_added.ok()) << *tunnel_added.error();
    const std::string tunnel_id_text =
        require_result(tunnel_added).at("tunnel").at("id").get<std::string>();
    auto tunnel_id = common::Id::parse(tunnel_id_text, common::IdKind::tunnel);
    ASSERT_TRUE(tunnel_id) << tunnel_id.error();

    const auto removed = dispatch(dispatcher_, "server.remove",
                                  ipc::Json{{"identifier", "primary"}});
    ASSERT_TRUE(removed.ok()) << *removed.error();

    const auto listed_servers = dispatch(dispatcher_, "server.list", ipc::Json::object());
    const auto listed_tunnels = dispatch(dispatcher_, "tun.list", ipc::Json::object());
    ASSERT_TRUE(listed_servers.ok()) << *listed_servers.error();
    ASSERT_TRUE(listed_tunnels.ok()) << *listed_tunnels.error();
    EXPECT_TRUE(require_result(listed_servers).at("servers").empty());
    EXPECT_TRUE(require_result(listed_tunnels).at("tunnels").empty());

    const auto persisted_server = repository_->servers().get_by_id(*server_id);
    const auto persisted_tunnel = repository_->tunnels().get_by_id(*tunnel_id);
    ASSERT_TRUE(persisted_server) << persisted_server.error();
    ASSERT_TRUE(persisted_tunnel) << persisted_tunnel.error();
    EXPECT_EQ(persisted_server->desired_state, storage::ServerDesiredState::removed);
    EXPECT_EQ(persisted_tunnel->desired_state, storage::TunnelDesiredState::removed);
    EXPECT_EQ(persisted_tunnel->actual_state, storage::TunnelActualState::removing);

    const auto missing_credential = credentials_->get(credential_key);
    ASSERT_FALSE(missing_credential);
    EXPECT_EQ(missing_credential.error().code(), common::ErrorCode::not_found);
}

TEST_F(DaemonControlServiceTest, RejectsUnknownFieldsAndInvalidPorts) {
    const auto unknown = dispatch(dispatcher_, "server.add",
                                  ipc::Json{{"endpoint", "example.com:2333"},
                                            {"unexpected", true}});
    ASSERT_FALSE(unknown.ok());
    ASSERT_NE(unknown.error(), nullptr);
    EXPECT_EQ(unknown.error()->code(), common::ErrorCode::invalid_argument);

    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"},
                                          {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const auto invalid_port = dispatch(
        dispatcher_, "tun.add",
        ipc::Json{{"server", "primary"}, {"local_port", 0}, {"remote_port", 6000}});
    ASSERT_FALSE(invalid_port.ok());
    ASSERT_NE(invalid_port.error(), nullptr);
    EXPECT_EQ(invalid_port.error()->code(), common::ErrorCode::invalid_argument);
}

TEST_F(DaemonControlServiceTest, HandlesConcurrentPersistentMutations) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"},
                                          {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();

    constexpr std::size_t kTunnelCount = 12U;
    std::atomic_size_t success_count{0U};
    std::vector<std::thread> workers;
    workers.reserve(kTunnelCount);
    for (std::size_t index = 0; index < kTunnelCount; ++index) {
        workers.emplace_back([this, index, &success_count] {
            const auto response = dispatch(
                dispatcher_, "tun.add",
                ipc::Json{{"server", "primary"},
                          {"local_port", 10'000 + static_cast<int>(index)},
                          {"remote_port", 20'000 + static_cast<int>(index)},
                          {"name", "tunnel-" + std::to_string(index)}});
            if (response.ok()) {
                success_count.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(success_count.load(std::memory_order_relaxed), kTunnelCount);

    const auto listed = dispatch(dispatcher_, "tun.list", ipc::Json::object());
    ASSERT_TRUE(listed.ok()) << *listed.error();
    EXPECT_EQ(require_result(listed).at("tunnels").size(), kTunnelCount);
}

} // namespace
} // namespace minitun::daemon
