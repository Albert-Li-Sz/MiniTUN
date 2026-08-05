#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/daemon/control_service.hpp>
#include <minitun/daemon/credential_keys.hpp>
#include <minitun/ipc/dispatcher.hpp>
#include <minitun/ipc/protocol.hpp>
#include <minitun/storage/credential_store.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

#include "storage_test_support.hpp"

namespace minitun::daemon {
namespace {

using storage::test::NativeSqliteDatabase;
using storage::test::TemporaryDatabaseFile;

class FailingCredentialStore final : public storage::CredentialStore {
  public:
    explicit FailingCredentialStore(storage::CredentialStore& delegate) noexcept
        : delegate_(delegate) {}

    void fail_next_remove() noexcept { fail_remove_ = true; }

    [[nodiscard]] common::Result<void> put(const std::string_view key,
                                           const std::string_view secret) override {
        return delegate_.put(key, secret);
    }

    [[nodiscard]] common::Result<common::SecureString>
    get(const std::string_view key) const override {
        return delegate_.get(key);
    }

    [[nodiscard]] common::Result<void> remove(const std::string_view key) override {
        if (fail_remove_) {
            fail_remove_ = false;
            return common::Error{common::ErrorCode::database_error,
                                 "injected credential cleanup failure"};
        }
        return delegate_.remove(key);
    }

  private:
    storage::CredentialStore& delegate_;
    bool fail_remove_{false};
};

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

        service_ = std::make_unique<ControlService>(
            *repository_, *credentials_,
            [this] { notifications_.fetch_add(1U, std::memory_order_relaxed); },
            [] {
                return ipc::Json{{"sessions", ipc::Json{{"active", 2}}},
                                 {"workers", ipc::Json{{"idle", 3}, {"active", 1}}},
                                 {"connections", ipc::Json{{"active", 1}, {"pending", 0}}},
                                 {"reconnects", 4},
                                 {"quota_rejections", 0},
                                 {"errors", 0},
                                 {"throughput", ipc::Json{{"bytes_in", 0}, {"bytes_out", 0}}}};
            },
            [this] {
                reloads_.fetch_add(1U, std::memory_order_relaxed);
                return common::Result<void>::success();
            });
        const auto registered = service_->register_handlers(dispatcher_);
        ASSERT_TRUE(registered) << registered.error();
    }

    TemporaryDatabaseFile state_file_;
    TemporaryDatabaseFile credential_file_;
    std::unique_ptr<storage::StateRepository> repository_;
    std::unique_ptr<storage::SqliteCredentialStore> credentials_;
    std::unique_ptr<ControlService> service_;
    ipc::Dispatcher dispatcher_;
    std::atomic_size_t notifications_{0U};
    std::atomic_size_t reloads_{0U};
};

TEST_F(DaemonControlServiceTest, NotifiesOnlyAfterSuccessfulStateMutations) {
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), 0U);
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), 1U);

    const auto inspected =
        dispatch(dispatcher_, "server.inspect", ipc::Json{{"identifier", "primary"}});
    ASSERT_TRUE(inspected.ok()) << *inspected.error();
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), 1U);

    const auto logged_in = dispatch(dispatcher_, "server.login",
                                    ipc::Json{{"identifier", "primary"}, {"token", "token"}});
    ASSERT_TRUE(logged_in.ok()) << *logged_in.error();
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), 2U);

    const auto tunnel_added = dispatch(
        dispatcher_, "tun.add",
        ipc::Json{
            {"server", "primary"}, {"local_port", 22}, {"remote_port", 6000}, {"name", "ssh"}});
    ASSERT_TRUE(tunnel_added.ok()) << *tunnel_added.error();
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), 3U);

    const auto tunnel_removed =
        dispatch(dispatcher_, "tun.remove", ipc::Json{{"identifier", "ssh"}});
    ASSERT_TRUE(tunnel_removed.ok()) << *tunnel_removed.error();
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), 4U);

    const auto server_removed =
        dispatch(dispatcher_, "server.remove", ipc::Json{{"identifier", "primary"}});
    ASSERT_TRUE(server_removed.ok()) << *server_removed.error();
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), 5U);

    const auto duplicate_remove =
        dispatch(dispatcher_, "server.remove", ipc::Json{{"identifier", "primary"}});
    EXPECT_FALSE(duplicate_remove.ok());
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), 5U);
}

TEST_F(DaemonControlServiceTest, RegistersCompleteStageFourMethodSet) {
    EXPECT_EQ(dispatcher_.size(), 16U);

    const auto daemon_status = dispatch(dispatcher_, "daemon.status", ipc::Json::object());
    ASSERT_TRUE(daemon_status.ok());
    EXPECT_EQ(require_result(daemon_status).at("state"), "running");

    const auto status = dispatch(dispatcher_, "status", ipc::Json::object());
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(require_result(status).at("servers").at("total"), 0);
    EXPECT_EQ(require_result(status).at("tunnels").at("total"), 0);
    EXPECT_EQ(require_result(status).at("runtime").at("sessions").at("active"), 2);
}

TEST_F(DaemonControlServiceTest, ExposesHealthReadinessMetricsDoctorAndReload) {
    const auto health = dispatch(dispatcher_, "health", ipc::Json::object());
    ASSERT_TRUE(health.ok()) << *health.error();
    EXPECT_EQ(require_result(health).at("status"), "ok");
    EXPECT_TRUE(require_result(health).at("state_db").get<bool>());
    EXPECT_TRUE(require_result(health).at("credentials_db").get<bool>());

    const auto readiness = dispatch(dispatcher_, "readiness", ipc::Json::object());
    ASSERT_TRUE(readiness.ok()) << *readiness.error();
    EXPECT_TRUE(require_result(readiness).at("ready").get<bool>());
    EXPECT_TRUE(require_result(readiness).at("reason").is_null());

    const auto metrics = dispatch(dispatcher_, "metrics", ipc::Json::object());
    ASSERT_TRUE(metrics.ok()) << *metrics.error();
    EXPECT_EQ(require_result(metrics).at("sessions").at("active"), 2);
    EXPECT_EQ(require_result(metrics).at("workers").at("idle"), 3);
    EXPECT_EQ(require_result(metrics).at("reconnects"), 4);

    const auto doctor = dispatch(dispatcher_, "doctor", ipc::Json::object());
    ASSERT_TRUE(doctor.ok()) << *doctor.error();
    EXPECT_TRUE(require_result(doctor).at("ok").get<bool>());
    EXPECT_TRUE(require_result(doctor).at("state_db").at("schema_valid").get<bool>());
    EXPECT_TRUE(require_result(doctor).at("credentials_db").at("integrity_ok").get<bool>());

    EXPECT_EQ(reloads_.load(std::memory_order_relaxed), 0U);
    const auto reload = dispatch(dispatcher_, "reload", ipc::Json::object());
    ASSERT_TRUE(reload.ok()) << *reload.error();
    EXPECT_TRUE(require_result(reload).at("reloaded").get<bool>());
    EXPECT_EQ(reloads_.load(std::memory_order_relaxed), 1U);
}

TEST_F(DaemonControlServiceTest, PrevalidatesBothRestoreSourcesBeforeChangingLiveState) {
    auto live_client_id = repository_->client_id();
    ASSERT_TRUE(live_client_id) << live_client_id.error();

    TemporaryDatabaseFile restore_state_file;
    auto restore_repository = storage::StateRepository::open(restore_state_file.path_string());
    ASSERT_TRUE(restore_repository) << restore_repository.error();
    auto restore_client_id = (*restore_repository)->client_id();
    ASSERT_TRUE(restore_client_id) << restore_client_id.error();
    ASSERT_NE(*restore_client_id, *live_client_id);
    const auto state_backup = restore_state_file.directory() / "restore-state.db";
    ASSERT_TRUE((*restore_repository)->backup_to(state_backup.string()));

    TemporaryDatabaseFile invalid_credentials_file;
    {
        NativeSqliteDatabase invalid_credentials{invalid_credentials_file.path()};
        invalid_credentials.execute("CREATE TABLE unrelated(value TEXT NOT NULL)");
    }
    ASSERT_EQ(::chmod(invalid_credentials_file.path().c_str(), 0600), 0);

    const auto restored =
        dispatch(dispatcher_, "doctor",
                 ipc::Json{{"restore_state", state_backup.string()},
                           {"restore_credentials", invalid_credentials_file.path_string()}});
    ASSERT_FALSE(restored.ok());
    ASSERT_NE(restored.error(), nullptr);
    EXPECT_EQ(restored.error()->code(), common::ErrorCode::unsupported_version);
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), 0U);

    auto current_client_id = repository_->client_id();
    ASSERT_TRUE(current_client_id) << current_client_id.error();
    EXPECT_EQ(*current_client_id, *live_client_id);
}

TEST_F(DaemonControlServiceTest, PersistsServerLoginAndOfflineTunnelWithoutLeakingToken) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
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
        ipc::Json{
            {"server", "primary"}, {"local_port", 22}, {"remote_port", 6000}, {"name", "ssh"}});
    ASSERT_TRUE(tunnel_added.ok()) << *tunnel_added.error();
    const auto& tunnel = require_result(tunnel_added).at("tunnel");
    EXPECT_EQ(tunnel.at("desired_state"), "active");
    EXPECT_EQ(tunnel.at("actual_state"), "pending");
    EXPECT_TRUE(tunnel.at("last_synced_at").is_null());
    EXPECT_EQ(tunnel.at("server_id"), server_id_text);

    const auto status = dispatch(dispatcher_, "status", ipc::Json::object());
    ASSERT_TRUE(status.ok()) << *status.error();
    EXPECT_EQ(require_result(status).at("servers").at("total"), 1);
    EXPECT_EQ(require_result(status).at("tunnels").at("total"), 1);
    EXPECT_EQ(require_result(status).at("tunnels").at("active"), 0);
}

TEST_F(DaemonControlServiceTest, ServerRemovalCleansLegacyAndManagedCredentialKeys) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    auto server_id = common::Id::parse(
        require_result(added).at("server").at("id").get<std::string>(), common::IdKind::server);
    ASSERT_TRUE(server_id) << server_id.error();

    constexpr std::string_view legacy_key = "legacy/server-token";
    ASSERT_TRUE(credentials_->put(legacy_key, "legacy-token"));
    const auto managed_keys = managed_credential_keys(*server_id);
    ASSERT_TRUE(credentials_->put(managed_keys[0], "primary-orphan"));
    ASSERT_TRUE(credentials_->put(managed_keys[1], "secondary-orphan"));
    auto server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(server) << server.error();
    server->credential_ref = std::string{legacy_key};
    ++server->updated_at_unix_ms;
    ASSERT_TRUE(repository_->servers().update(*server));

    const auto removed =
        dispatch(dispatcher_, "server.remove", ipc::Json{{"identifier", "primary"}});
    ASSERT_TRUE(removed.ok()) << *removed.error();
    for (const auto& key : {std::string{legacy_key}, managed_keys[0], managed_keys[1]}) {
        const auto missing = credentials_->get(key);
        ASSERT_FALSE(missing);
        EXPECT_EQ(missing.error().code(), common::ErrorCode::not_found);
    }
}

TEST_F(DaemonControlServiceTest, RotatesCredentialSlotsWithoutExposingThePreviousToken) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    auto server_id = common::Id::parse(
        require_result(added).at("server").at("id").get<std::string>(), common::IdKind::server);
    ASSERT_TRUE(server_id) << server_id.error();

    const auto first = dispatch(dispatcher_, "server.login",
                                ipc::Json{{"identifier", "primary"}, {"token", "first-token"}});
    ASSERT_TRUE(first.ok()) << *first.error();
    auto first_server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(first_server) << first_server.error();
    ASSERT_TRUE(first_server->credential_ref.has_value());
    const std::string first_key = *first_server->credential_ref;

    const auto second = dispatch(dispatcher_, "server.login",
                                 ipc::Json{{"identifier", "primary"}, {"token", "second-token"}});
    ASSERT_TRUE(second.ok()) << *second.error();
    auto second_server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(second_server) << second_server.error();
    ASSERT_TRUE(second_server->credential_ref.has_value());
    const std::string second_key = *second_server->credential_ref;
    EXPECT_NE(second_key, first_key);
    auto second_token = credentials_->get(second_key);
    ASSERT_TRUE(second_token) << second_token.error();
    EXPECT_EQ(second_token->view(), "second-token");
    const auto removed_first = credentials_->get(first_key);
    ASSERT_FALSE(removed_first);
    EXPECT_EQ(removed_first.error().code(), common::ErrorCode::not_found);

    const auto third = dispatch(dispatcher_, "server.login",
                                ipc::Json{{"identifier", "primary"}, {"token", "third-token"}});
    ASSERT_TRUE(third.ok()) << *third.error();
    auto third_server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(third_server) << third_server.error();
    ASSERT_TRUE(third_server->credential_ref.has_value());
    EXPECT_EQ(*third_server->credential_ref, first_key);
    auto third_token = credentials_->get(first_key);
    ASSERT_TRUE(third_token) << third_token.error();
    EXPECT_EQ(third_token->view(), "third-token");
    const auto removed_second = credentials_->get(second_key);
    ASSERT_FALSE(removed_second);
    EXPECT_EQ(removed_second.error().code(), common::ErrorCode::not_found);
}

TEST_F(DaemonControlServiceTest, KeepsCommittedLoginWhenOldCredentialCleanupIsDeferred) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    auto server_id = common::Id::parse(
        require_result(added).at("server").at("id").get<std::string>(), common::IdKind::server);
    ASSERT_TRUE(server_id) << server_id.error();
    ASSERT_TRUE(dispatch(dispatcher_, "server.login",
                         ipc::Json{{"identifier", "primary"}, {"token", "first-token"}})
                    .ok());
    auto before = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(before) << before.error();
    ASSERT_TRUE(before->credential_ref.has_value());
    const std::string previous_key = *before->credential_ref;

    FailingCredentialStore failing_store{*credentials_};
    failing_store.fail_next_remove();
    ControlService failing_service{*repository_, failing_store};
    ipc::Dispatcher failing_dispatcher;
    ASSERT_TRUE(failing_service.register_handlers(failing_dispatcher));
    const auto updated =
        dispatch(failing_dispatcher, "server.login",
                 ipc::Json{{"identifier", "primary"}, {"token", "replacement-token"}});
    ASSERT_TRUE(updated.ok()) << *updated.error();

    auto after = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(after) << after.error();
    ASSERT_TRUE(after->credential_ref.has_value());
    EXPECT_NE(*after->credential_ref, previous_key);
    auto replacement = credentials_->get(*after->credential_ref);
    ASSERT_TRUE(replacement) << replacement.error();
    EXPECT_EQ(replacement->view(), "replacement-token");
    auto retained_previous = credentials_->get(previous_key);
    ASSERT_TRUE(retained_previous) << retained_previous.error();
    EXPECT_EQ(retained_previous->view(), "first-token");

    const auto healed = dispatch(dispatcher_, "server.login",
                                 ipc::Json{{"identifier", "primary"}, {"token", "final-token"}});
    ASSERT_TRUE(healed.ok()) << *healed.error();
    auto final_server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(final_server) << final_server.error();
    ASSERT_TRUE(final_server->credential_ref.has_value());
    EXPECT_EQ(*final_server->credential_ref, previous_key);
    auto final_token = credentials_->get(previous_key);
    ASSERT_TRUE(final_token) << final_token.error();
    EXPECT_EQ(final_token->view(), "final-token");
}

TEST_F(DaemonControlServiceTest, ConcurrentLoginsKeepTheCommittedCredentialSlot) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    auto server_id = common::Id::parse(
        require_result(added).at("server").at("id").get<std::string>(), common::IdKind::server);
    ASSERT_TRUE(server_id) << server_id.error();

    constexpr std::size_t kLoginCount = 12U;
    std::atomic_size_t success_count{0U};
    std::vector<std::thread> workers;
    workers.reserve(kLoginCount);
    for (std::size_t index = 0; index < kLoginCount; ++index) {
        workers.emplace_back([this, index, &success_count] {
            const auto response =
                dispatch(dispatcher_, "server.login",
                         ipc::Json{{"identifier", "primary"},
                                   {"token", "concurrent-token-" + std::to_string(index)}});
            if (response.ok()) {
                success_count.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(success_count.load(std::memory_order_relaxed), kLoginCount);

    const auto server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(server) << server.error();
    ASSERT_TRUE(server->credential_ref.has_value());
    const auto keys = managed_credential_keys(*server_id);
    ASSERT_TRUE(*server->credential_ref == keys[0] || *server->credential_ref == keys[1]);
    for (const auto& key : keys) {
        const auto credential = credentials_->get(key);
        if (key == *server->credential_ref) {
            EXPECT_TRUE(credential) << credential.error();
        } else {
            ASSERT_FALSE(credential);
            EXPECT_EQ(credential.error().code(), common::ErrorCode::not_found);
        }
    }
}

TEST_F(DaemonControlServiceTest, PendingTunnelExplainsMissingServerAuthentication) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();

    const auto tunnel_added = dispatch(
        dispatcher_, "tun.add",
        ipc::Json{
            {"server", "primary"}, {"local_port", 8888}, {"remote_port", 82}, {"name", "web81"}});
    ASSERT_TRUE(tunnel_added.ok()) << *tunnel_added.error();
    const auto& tunnel = require_result(tunnel_added).at("tunnel");
    EXPECT_EQ(tunnel.at("actual_state"), "pending");
    EXPECT_EQ(tunnel.at("server_actual_state"), "not_authenticated");
    EXPECT_EQ(tunnel.at("pending_reason"), "server_not_authenticated");
    EXPECT_TRUE(tunnel.at("last_synced_at").is_null());
    ASSERT_TRUE(tunnel.at("last_error").is_object());
    EXPECT_EQ(tunnel.at("last_error").at("code"), "not_authenticated");
    EXPECT_EQ(tunnel.at("last_error").at("message"), "server credentials are not configured");
}

TEST_F(DaemonControlServiceTest, ServerRemovalDeletesStateAndCredentialImmediately) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
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
        ipc::Json{
            {"server", "primary"}, {"local_port", 22}, {"remote_port", 6000}, {"name", "ssh"}});
    ASSERT_TRUE(tunnel_added.ok()) << *tunnel_added.error();
    const std::string tunnel_id_text =
        require_result(tunnel_added).at("tunnel").at("id").get<std::string>();
    auto tunnel_id = common::Id::parse(tunnel_id_text, common::IdKind::tunnel);
    ASSERT_TRUE(tunnel_id) << tunnel_id.error();

    const auto removed =
        dispatch(dispatcher_, "server.remove", ipc::Json{{"identifier", "primary"}});
    ASSERT_TRUE(removed.ok()) << *removed.error();

    const auto listed_servers = dispatch(dispatcher_, "server.list", ipc::Json::object());
    const auto listed_tunnels = dispatch(dispatcher_, "tun.list", ipc::Json::object());
    ASSERT_TRUE(listed_servers.ok()) << *listed_servers.error();
    ASSERT_TRUE(listed_tunnels.ok()) << *listed_tunnels.error();
    EXPECT_TRUE(require_result(listed_servers).at("servers").empty());
    EXPECT_TRUE(require_result(listed_tunnels).at("tunnels").empty());

    const auto persisted_server = repository_->servers().get_by_id(*server_id);
    const auto persisted_tunnel = repository_->tunnels().get_by_id(*tunnel_id);
    ASSERT_FALSE(persisted_server);
    ASSERT_FALSE(persisted_tunnel);
    EXPECT_EQ(persisted_server.error().code(), common::ErrorCode::not_found);
    EXPECT_EQ(persisted_tunnel.error().code(), common::ErrorCode::not_found);

    const auto missing_credential = credentials_->get(credential_key);
    ASSERT_FALSE(missing_credential);
    EXPECT_EQ(missing_credential.error().code(), common::ErrorCode::not_found);

    const auto reused = dispatch(dispatcher_, "server.add",
                                 ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(reused.ok()) << *reused.error();
    EXPECT_NE(require_result(reused).at("server").at("id").get<std::string>(), server_id_text);
}

TEST_F(DaemonControlServiceTest, ServerRemovalSucceedsWhileExternalReaderPinsTheWal) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const std::string server_id_text =
        require_result(added).at("server").at("id").get<std::string>();
    auto server_id = common::Id::parse(server_id_text, common::IdKind::server);
    ASSERT_TRUE(server_id) << server_id.error();
    ASSERT_TRUE(dispatch(dispatcher_, "server.login",
                         ipc::Json{{"identifier", "primary"}, {"token", "token"}})
                    .ok());
    auto server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(server) << server.error();
    ASSERT_TRUE(server->credential_ref.has_value());
    const std::string credential_key = *server->credential_ref;

    ASSERT_TRUE(repository_->checkpoint());
    NativeSqliteDatabase external_reader{state_file_.path(),
                                         SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    external_reader.execute("BEGIN");
    EXPECT_EQ(external_reader.query_int64("SELECT COUNT(*) FROM servers"), 1);

    const auto removed =
        dispatch(dispatcher_, "server.remove", ipc::Json{{"identifier", "primary"}});
    ASSERT_TRUE(removed.ok()) << *removed.error();
    const auto missing_server = repository_->servers().get_by_id(*server_id);
    ASSERT_FALSE(missing_server);
    EXPECT_EQ(missing_server.error().code(), common::ErrorCode::not_found);
    const auto missing_credential = credentials_->get(credential_key);
    ASSERT_FALSE(missing_credential);
    EXPECT_EQ(missing_credential.error().code(), common::ErrorCode::not_found);

    // The reader keeps its pre-removal snapshot, while canonical WAL-aware reads
    // already observe the committed deletion.
    EXPECT_EQ(external_reader.query_int64("SELECT COUNT(*) FROM servers"), 1);
    external_reader.execute("COMMIT");
    EXPECT_TRUE(repository_->checkpoint());
}

TEST_F(DaemonControlServiceTest, TunnelRemovalDeletesStateAndAllowsImmediateReuse) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();

    const auto first = dispatch(
        dispatcher_, "tun.add",
        ipc::Json{
            {"server", "primary"}, {"local_port", 22}, {"remote_port", 6000}, {"name", "ssh"}});
    ASSERT_TRUE(first.ok()) << *first.error();
    const std::string first_id_text =
        require_result(first).at("tunnel").at("id").get<std::string>();
    auto first_id = common::Id::parse(first_id_text, common::IdKind::tunnel);
    ASSERT_TRUE(first_id) << first_id.error();

    const auto removed = dispatch(dispatcher_, "tun.remove", ipc::Json{{"identifier", "ssh"}});
    ASSERT_TRUE(removed.ok()) << *removed.error();
    const auto missing = repository_->tunnels().get_by_id(*first_id);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), common::ErrorCode::not_found);

    const auto reused = dispatch(
        dispatcher_, "tun.add",
        ipc::Json{
            {"server", "primary"}, {"local_port", 2222}, {"remote_port", 6000}, {"name", "ssh"}});
    ASSERT_TRUE(reused.ok()) << *reused.error();
    EXPECT_NE(require_result(reused).at("tunnel").at("id").get<std::string>(), first_id_text);
}

TEST_F(DaemonControlServiceTest, RejectsUnknownFieldsAndInvalidPorts) {
    const auto unknown =
        dispatch(dispatcher_, "server.add",
                 ipc::Json{{"endpoint", "example.com:2333"}, {"unexpected", true}});
    ASSERT_FALSE(unknown.ok());
    ASSERT_NE(unknown.error(), nullptr);
    EXPECT_EQ(unknown.error()->code(), common::ErrorCode::invalid_argument);

    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const auto invalid_port =
        dispatch(dispatcher_, "tun.add",
                 ipc::Json{{"server", "primary"}, {"local_port", 0}, {"remote_port", 6000}});
    ASSERT_FALSE(invalid_port.ok());
    ASSERT_NE(invalid_port.error(), nullptr);
    EXPECT_EQ(invalid_port.error()->code(), common::ErrorCode::invalid_argument);
}

TEST_F(DaemonControlServiceTest, HandlesConcurrentPersistentMutations) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();

    constexpr std::size_t kTunnelCount = 12U;
    std::atomic_size_t success_count{0U};
    std::vector<std::thread> workers;
    workers.reserve(kTunnelCount);
    for (std::size_t index = 0; index < kTunnelCount; ++index) {
        workers.emplace_back([this, index, &success_count] {
            const auto response =
                dispatch(dispatcher_, "tun.add",
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
