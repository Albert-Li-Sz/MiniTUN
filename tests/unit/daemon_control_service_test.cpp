#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
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

void write_test_file(const std::filesystem::path& path, const std::string_view contents,
                     const mode_t mode = 0600) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        throw std::runtime_error("failed to open a test input file");
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.close();
    if (!stream || ::chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error("failed to prepare a test input file");
    }
}

void expect_control_error(const ipc::Response& response,
                          const common::ErrorCode expected = common::ErrorCode::invalid_argument) {
    ASSERT_FALSE(response.ok());
    ASSERT_NE(response.error(), nullptr);
    EXPECT_EQ(response.error()->code(), expected) << response.error()->message();
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

    const auto logged_in =
        dispatch(dispatcher_, "server.login", ipc::Json{{"identifier", "primary"}, {"psk", "psk"}});
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
    EXPECT_EQ(dispatcher_.size(), 27U);

    const auto daemon_status = dispatch(dispatcher_, "daemon.status", ipc::Json::object());
    ASSERT_TRUE(daemon_status.ok());
    EXPECT_EQ(require_result(daemon_status).at("state"), "running");

    const auto daemon_identity = dispatch(dispatcher_, "daemon.identity", ipc::Json::object());
    ASSERT_TRUE(daemon_identity.ok());
    EXPECT_TRUE(
        common::Id::parse(require_result(daemon_identity).at("client_id").get<std::string>(),
                          common::IdKind::client));

    const auto status = dispatch(dispatcher_, "status", ipc::Json::object());
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(require_result(status).at("servers").at("total"), 0);
    EXPECT_EQ(require_result(status).at("tunnels").at("total"), 0);
    EXPECT_EQ(require_result(status).at("runtime").at("sessions").at("active"), 2);
}

TEST_F(DaemonControlServiceTest, DeclarativePlanApplyIsStableAndPrunesOnlyManagedResources) {
    ASSERT_TRUE(dispatch(dispatcher_, "server.add",
                         ipc::Json{{"endpoint", "manual.example.com:2333"}, {"name", "manual"}})
                    .ok());
    const auto config_path = state_file_.directory() / "minitun-config.json";
    const auto write_config = [&config_path](const std::string_view contents) {
        std::ofstream stream{config_path, std::ios::binary | std::ios::trunc};
        stream << contents;
        stream.close();
        ASSERT_TRUE(stream);
        ASSERT_EQ(::chmod(config_path.c_str(), 0600), 0);
    };
    write_config(R"json({
        "format_version": 1,
        "servers": [{
            "name": "edge",
            "endpoint": "edge.example.com:2443",
            "tls_server_name": "tunnel.example.com",
            "enabled": true
        }],
        "tunnels": [{
            "name": "ssh",
            "server": "edge",
            "local_host": "127.0.0.1",
            "local_port": 22,
            "remote_port": 6022,
            "enabled": true
        }]
    })json");

    const auto planned =
        dispatch(dispatcher_, "config.plan", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(planned.ok()) << *planned.error();
    ASSERT_EQ(require_result(planned).at("actions").size(), 2U) << require_result(planned).dump(2);
    EXPECT_EQ(require_result(planned).at("actions").at(0).at("resource"), "server");
    EXPECT_EQ(require_result(planned).at("actions").at(0).at("action"), "create");
    EXPECT_EQ(require_result(planned).at("actions").at(1).at("resource"), "tunnel");

    const auto applied =
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(applied.ok()) << *applied.error();
    EXPECT_EQ(require_result(applied).at("changed"), 2U);
    const auto notifications_after_apply = notifications_.load(std::memory_order_relaxed);

    const auto repeated =
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(repeated.ok()) << *repeated.error();
    EXPECT_EQ(require_result(repeated).at("changed"), 0U);
    EXPECT_TRUE(require_result(repeated).at("actions").empty());
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_after_apply);

    const auto exported = dispatch(dispatcher_, "config.export", ipc::Json::object());
    ASSERT_TRUE(exported.ok()) << *exported.error();
    const std::string export_text = require_result(exported).dump();
    EXPECT_EQ(export_text.find("_file"), std::string::npos);
    EXPECT_EQ(export_text.find(config_path.string()), std::string::npos);
    EXPECT_EQ(require_result(exported).at("servers").size(), 2U);
    EXPECT_EQ(require_result(exported).at("tunnels").size(), 1U);

    write_config(R"json({"format_version":1,"servers":[],"tunnels":[]})json");
    const auto without_prune =
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(without_prune.ok()) << *without_prune.error();
    EXPECT_EQ(require_result(without_prune).at("changed"), 0U);

    const auto pruned = dispatch(dispatcher_, "config.apply",
                                 ipc::Json{{"path", config_path.string()}, {"prune", true}});
    ASSERT_TRUE(pruned.ok()) << *pruned.error();
    EXPECT_EQ(require_result(pruned).at("changed"), 2U);
    const auto servers = dispatch(dispatcher_, "server.list", ipc::Json::object());
    const auto tunnels = dispatch(dispatcher_, "tun.list", ipc::Json::object());
    ASSERT_TRUE(servers.ok()) << *servers.error();
    ASSERT_TRUE(tunnels.ok()) << *tunnels.error();
    ASSERT_EQ(require_result(servers).at("servers").size(), 1U);
    EXPECT_EQ(require_result(servers).at("servers").at(0).at("name"), "manual");
    EXPECT_TRUE(require_result(tunnels).at("tunnels").empty());
}

TEST_F(DaemonControlServiceTest, DeclarativeConfigurationRejectsDuplicateJsonKeys) {
    const auto config_path = state_file_.directory() / "duplicate-config.json";
    std::ofstream stream{config_path, std::ios::binary | std::ios::trunc};
    stream << R"json({"format_version":1,"format_version":1,"servers":[],"tunnels":[]})json";
    stream.close();
    ASSERT_TRUE(stream);
    ASSERT_EQ(::chmod(config_path.c_str(), 0600), 0);
    const auto planned =
        dispatch(dispatcher_, "config.plan", ipc::Json{{"path", config_path.string()}});
    ASSERT_FALSE(planned.ok());
    ASSERT_NE(planned.error(), nullptr);
    EXPECT_EQ(planned.error()->code(), common::ErrorCode::invalid_argument);
}

TEST_F(DaemonControlServiceTest, DeclarativeConfigurationRejectsMalformedDocumentsAndResources) {
    auto server_id = common::Id::generate(common::IdKind::server);
    auto tunnel_id = common::Id::generate(common::IdKind::tunnel);
    ASSERT_TRUE(server_id) << server_id.error();
    ASSERT_TRUE(tunnel_id) << tunnel_id.error();

    struct InvalidConfiguration final {
        std::string name;
        std::string contents;
        common::ErrorCode error{common::ErrorCode::invalid_argument};
    };
    const std::string valid_server =
        R"json({"name":"edge","endpoint":"edge.example.com:2333"})json";
    const std::vector<InvalidConfiguration> cases{
        {"syntax", "not-json"},
        {"root-array", "[]"},
        {"empty-root", "{}"},
        {"unknown-root-field",
         R"json({"format_version":1,"servers":[],"tunnels":[],"extra":true})json"},
        {"wrong-version", R"json({"format_version":2,"servers":[],"tunnels":[]})json"},
        {"signed-version", R"json({"format_version":-1,"servers":[],"tunnels":[]})json"},
        {"string-version", R"json({"format_version":"1","servers":[],"tunnels":[]})json"},
        {"missing-servers", R"json({"format_version":1,"tunnels":[]})json"},
        {"missing-tunnels", R"json({"format_version":1,"servers":[]})json"},
        {"servers-not-array", R"json({"format_version":1,"servers":{},"tunnels":[]})json"},
        {"tunnels-not-array", R"json({"format_version":1,"servers":[],"tunnels":{}})json"},
        {"server-not-object", R"json({"format_version":1,"servers":[false],"tunnels":[]})json"},
        {"server-unknown-field",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"edge.example.com:2333","extra":1}],"tunnels":[]})json"},
        {"server-no-identity",
         R"json({"format_version":1,"servers":[{"endpoint":"edge.example.com:2333"}],"tunnels":[]})json"},
        {"server-missing-endpoint",
         R"json({"format_version":1,"servers":[{"name":"edge"}],"tunnels":[]})json"},
        {"server-null-endpoint",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":null}],"tunnels":[]})json"},
        {"server-empty-name",
         R"json({"format_version":1,"servers":[{"name":"","endpoint":"edge.example.com:2333"}],"tunnels":[]})json"},
        {"server-name-type",
         R"json({"format_version":1,"servers":[{"name":false,"endpoint":"edge.example.com:2333"}],"tunnels":[]})json"},
        {"server-endpoint-type",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":1}],"tunnels":[]})json"},
        {"server-invalid-endpoint",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"missing-port"}],"tunnels":[]})json"},
        {"server-invalid-id",
         R"json({"format_version":1,"servers":[{"id":"tun_wrong","endpoint":"edge.example.com:2333"}],"tunnels":[]})json"},
        {"server-tls-type",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"edge.example.com:2333","tls_server_name":7}],"tunnels":[]})json"},
        {"server-enabled-type",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"edge.example.com:2333","enabled":"yes"}],"tunnels":[]})json"},
        {"duplicate-server-name",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"one.example.com:1"},{"name":"edge","endpoint":"two.example.com:2"}],"tunnels":[]})json",
         common::ErrorCode::already_exists},
        {"duplicate-server-id",
         "{\"format_version\":1,\"servers\":[{\"id\":\"" + server_id->str() +
             "\",\"endpoint\":\"one.example.com:1\"},{\"id\":\"" + server_id->str() +
             "\",\"endpoint\":\"two.example.com:2\"}],\"tunnels\":[]}",
         common::ErrorCode::already_exists},
        {"credential-path-type",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"edge.example.com:2333","psk_file":5}],"tunnels":[]})json"},
        {"credential-marker-not-object",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"edge.example.com:2333","credentials":true}],"tunnels":[]})json"},
        {"credential-marker-unknown",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"edge.example.com:2333","credentials":{"token":false}}],"tunnels":[]})json"},
        {"credential-marker-type",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"edge.example.com:2333","credentials":{"psk":"false"}}],"tunnels":[]})json"},
        {"credential-marker-mismatch",
         R"json({"format_version":1,"servers":[{"name":"edge","endpoint":"edge.example.com:2333","credentials":{"psk":true}}],"tunnels":[]})json"},
        {"tunnel-not-object",
         "{\"format_version\":1,\"servers\":[" + valid_server + "],\"tunnels\":[false]}",
         common::ErrorCode::invalid_argument},
        {"tunnel-unknown-field",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"edge","local_port":22,"remote_port":6022,"extra":1}]})json"},
        {"tunnel-no-identity",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"server":"edge","local_port":22,"remote_port":6022}]})json"},
        {"tunnel-missing-server",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","local_port":22,"remote_port":6022}]})json"},
        {"tunnel-absent-parent",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"missing","local_port":22,"remote_port":6022}]})json",
         common::ErrorCode::not_found},
        {"tunnel-invalid-id",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"id":"srv_wrong","server":"edge","local_port":22,"remote_port":6022}]})json"},
        {"tunnel-name-type",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":3,"server":"edge","local_port":22,"remote_port":6022}]})json"},
        {"tunnel-local-host-type",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"edge","local_host":7,"local_port":22,"remote_port":6022}]})json"},
        {"tunnel-enabled-type",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"edge","local_port":22,"remote_port":6022,"enabled":1}]})json"},
        {"tunnel-missing-local-port",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"edge","remote_port":6022}]})json"},
        {"tunnel-negative-local-port",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"edge","local_port":-1,"remote_port":6022}]})json"},
        {"tunnel-zero-local-port",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"edge","local_port":0,"remote_port":6022}]})json"},
        {"tunnel-large-local-port",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"edge","local_port":65536,"remote_port":6022}]})json"},
        {"tunnel-string-remote-port",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"edge","local_port":22,"remote_port":"6022"}]})json"},
        {"duplicate-tunnel-name",
         "{\"format_version\":1,\"servers\":[" + valid_server +
             R"json(],"tunnels":[{"name":"ssh","server":"edge","local_port":22,"remote_port":6022},{"name":"ssh","server":"edge","local_port":23,"remote_port":6023}]})json",
         common::ErrorCode::already_exists},
        {"duplicate-tunnel-id",
         "{\"format_version\":1,\"servers\":[" + valid_server + "],\"tunnels\":[{\"id\":\"" +
             tunnel_id->str() +
             "\",\"server\":\"edge\",\"local_port\":22,\"remote_port\":6022},{\"id\":\"" +
             tunnel_id->str() + "\",\"server\":\"edge\",\"local_port\":23,\"remote_port\":6023}]}",
         common::ErrorCode::already_exists},
    };

    const auto config_path = state_file_.directory() / "invalid-config.json";
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        write_test_file(config_path, item.contents);
        const auto planned =
            dispatch(dispatcher_, "config.plan", ipc::Json{{"path", config_path.string()}});
        expect_control_error(planned, item.error);
        auto servers = repository_->servers().list();
        auto tunnels = repository_->tunnels().list();
        ASSERT_TRUE(servers) << servers.error();
        ASSERT_TRUE(tunnels) << tunnels.error();
        EXPECT_TRUE(servers->empty());
        EXPECT_TRUE(tunnels->empty());
    }
}

TEST_F(DaemonControlServiceTest, DeclarativeConfigurationEnforcesInputFileSafetyAndParseLimits) {
    const auto directory = state_file_.directory();
    const auto valid_path = directory / "valid-source.json";
    constexpr std::string_view valid = R"json({"format_version":1,"servers":[],"tunnels":[]})json";
    write_test_file(valid_path, valid);

    const auto expect_path_error = [this](const std::string& path, const common::ErrorCode code) {
        const auto response = dispatch(dispatcher_, "config.plan", ipc::Json{{"path", path}});
        expect_control_error(response, code);
    };
    expect_path_error("", common::ErrorCode::invalid_argument);
    expect_path_error(std::string(4'097U, 'x'), common::ErrorCode::invalid_argument);
    expect_path_error(std::string{"bad\0path", 8U}, common::ErrorCode::invalid_argument);
    expect_path_error((directory / "missing.json").string(), common::ErrorCode::invalid_argument);
    expect_path_error(directory.string(), common::ErrorCode::permission_denied);

    const auto empty_path = directory / "empty.json";
    write_test_file(empty_path, "");
    expect_path_error(empty_path.string(), common::ErrorCode::permission_denied);

    const auto writable_path = directory / "writable.json";
    write_test_file(writable_path, valid, 0666);
    expect_path_error(writable_path.string(), common::ErrorCode::permission_denied);

    const auto hardlink_path = directory / "hardlink.json";
    ASSERT_EQ(::link(valid_path.c_str(), hardlink_path.c_str()), 0);
    expect_path_error(hardlink_path.string(), common::ErrorCode::permission_denied);

    const auto symlink_path = directory / "symlink.json";
    ASSERT_EQ(::symlink(valid_path.c_str(), symlink_path.c_str()), 0);
    expect_path_error(symlink_path.string(), common::ErrorCode::invalid_argument);

    const auto nul_path = directory / "nul.json";
    write_test_file(nul_path, std::string{"{}\0{}", 5U});
    expect_path_error(nul_path.string(), common::ErrorCode::invalid_argument);

    const auto bom_path = directory / "bom.json";
    write_test_file(bom_path, std::string{"\xef\xbb\xbf", 3U} + std::string{valid});
    expect_path_error(bom_path.string(), common::ErrorCode::invalid_argument);

    const auto depth_path = directory / "depth.json";
    write_test_file(depth_path, "[[[[[[[[[[[[[]]]]]]]]]]]]]");
    expect_path_error(depth_path.string(), common::ErrorCode::resource_exhausted);

    const auto string_path = directory / "string-limit.json";
    write_test_file(string_path, "\"" + std::string(65'537U, 'x') + "\"");
    expect_path_error(string_path.string(), common::ErrorCode::resource_exhausted);

    const auto nodes_path = directory / "node-limit.json";
    std::string nodes{"["};
    nodes.reserve(200'010U);
    for (std::size_t index = 0U; index < 100'001U; ++index) {
        if (index != 0U) {
            nodes.push_back(',');
        }
        nodes.push_back('0');
    }
    nodes.push_back(']');
    write_test_file(nodes_path, nodes);
    expect_path_error(nodes_path.string(), common::ErrorCode::resource_exhausted);

    const auto oversized_path = directory / "oversized.json";
    write_test_file(oversized_path, std::string(4U * 1024U * 1024U + 1U, ' '));
    expect_path_error(oversized_path.string(), common::ErrorCode::resource_exhausted);
}

TEST_F(DaemonControlServiceTest, DeclarativeConfigurationMatchesStableIdsAndProtectsOwnership) {
    auto server_id = common::Id::generate(common::IdKind::server);
    auto second_server_id = common::Id::generate(common::IdKind::server);
    auto tunnel_id = common::Id::generate(common::IdKind::tunnel);
    ASSERT_TRUE(server_id) << server_id.error();
    ASSERT_TRUE(second_server_id) << second_server_id.error();
    ASSERT_TRUE(tunnel_id) << tunnel_id.error();

    const auto directory = state_file_.directory();
    const auto psk_path = directory / "stable.psk";
    const auto config_path = directory / "stable-config.json";
    write_test_file(psk_path, "stable-secret\r\n");
    const std::string first =
        "{\"format_version\":1,\"servers\":[{\"id\":\"" + server_id->str() +
        "\",\"name\":\"edge\",\"endpoint\":\"edge.example.com:2333\",\"enabled\":false,"
        "\"psk_file\":\"stable.psk\",\"credentials\":{\"psk\":true,\"ca\":false,"
        "\"client_certificate\":false}}],\"tunnels\":[{\"id\":\"" +
        tunnel_id->str() + "\",\"name\":\"ssh\",\"server\":\"" + server_id->str() +
        "\",\"local_host\":\"::1\",\"local_port\":22,\"remote_port\":6022,"
        "\"enabled\":false}]}";
    write_test_file(config_path, first);
    const auto applied =
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(applied.ok()) << *applied.error();
    EXPECT_EQ(require_result(applied).at("changed"), 2U);

    auto server = repository_->servers().get_by_id(*server_id);
    auto tunnel = repository_->tunnels().get_by_id(*tunnel_id);
    ASSERT_TRUE(server) << server.error();
    ASSERT_TRUE(tunnel) << tunnel.error();
    EXPECT_TRUE(server->managed_by_config);
    EXPECT_TRUE(tunnel->managed_by_config);
    EXPECT_EQ(server->actual_state, storage::ServerActualState::disabled);
    EXPECT_EQ(tunnel->actual_state, storage::TunnelActualState::disabled);
    EXPECT_EQ(tunnel->local_endpoint.host(), "::1");
    ASSERT_TRUE(server->credential_ref.has_value());
    auto psk = credentials_->get(*server->credential_ref);
    ASSERT_TRUE(psk) << psk.error();
    EXPECT_EQ(psk->view(), "stable-secret");
    const auto initial_server_revision = server->config_revision;
    const auto initial_tunnel_revision = tunnel->config_revision;

    const std::string update =
        "{\"format_version\":1,\"servers\":[{\"id\":\"" + server_id->str() +
        "\",\"name\":null,\"endpoint\":\"new.example.com:2443\",\"tls_server_name\":null,"
        "\"enabled\":true,\"psk_file\":null,\"credentials\":{\"psk\":false}}],"
        "\"tunnels\":[{\"id\":\"" +
        tunnel_id->str() + "\",\"name\":null,\"server\":\"" + server_id->str() +
        "\",\"local_host\":\"localhost\",\"local_port\":2222,\"remote_port\":6122,"
        "\"enabled\":true}]}";
    write_test_file(config_path, update);
    const auto planned =
        dispatch(dispatcher_, "config.plan", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(planned.ok()) << *planned.error();
    ASSERT_EQ(require_result(planned).at("actions").size(), 2U);
    EXPECT_EQ(require_result(planned).at("actions").at(0).at("id"), server_id->str());
    EXPECT_EQ(require_result(planned).at("actions").at(1).at("id"), tunnel_id->str());
    const auto updated =
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(updated.ok()) << *updated.error();

    server = repository_->servers().get_by_id(*server_id);
    tunnel = repository_->tunnels().get_by_id(*tunnel_id);
    ASSERT_TRUE(server) << server.error();
    ASSERT_TRUE(tunnel) << tunnel.error();
    EXPECT_FALSE(server->name.has_value());
    EXPECT_FALSE(server->credential_ref.has_value());
    EXPECT_EQ(server->config_revision, initial_server_revision + 1U);
    EXPECT_EQ(server->actual_state, storage::ServerActualState::not_authenticated);
    EXPECT_FALSE(tunnel->name.has_value());
    EXPECT_EQ(tunnel->config_revision, initial_tunnel_revision + 1U);
    EXPECT_EQ(tunnel->actual_state, storage::TunnelActualState::pending);

    const std::string moved =
        "{\"format_version\":1,\"servers\":[{\"id\":\"" + server_id->str() +
        "\",\"endpoint\":\"new.example.com:2443\"},{\"id\":\"" + second_server_id->str() +
        "\",\"endpoint\":\"second.example.com:2443\"}],\"tunnels\":[{\"id\":\"" + tunnel_id->str() +
        "\",\"server\":\"" + second_server_id->str() +
        "\",\"local_port\":2222,\"remote_port\":6122}]}";
    write_test_file(config_path, moved);
    expect_control_error(
        dispatch(dispatcher_, "config.plan", ipc::Json{{"path", config_path.string()}}));

    ASSERT_TRUE(dispatch(dispatcher_, "tun.add",
                         ipc::Json{{"server", server_id->str()},
                                   {"name", "manual-child"},
                                   {"local_port", 9000},
                                   {"remote_port", 9001}})
                    .ok());
    write_test_file(config_path, R"json({"format_version":1,"servers":[],"tunnels":[]})json");
    expect_control_error(dispatch(dispatcher_, "config.apply",
                                  ipc::Json{{"path", config_path.string()}, {"prune", true}}));
    ASSERT_TRUE(repository_->servers().get_by_id(*server_id));
}

TEST_F(DaemonControlServiceTest, DeclarativeCredentialsAreAtomicIdempotentAndNeverExported) {
    const auto config_path = state_file_.directory() / "credential-config.json";
    const auto psk_path = state_file_.directory() / "edge.psk";
    const auto write_private = [](const std::filesystem::path& path,
                                  const std::string_view contents) {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << contents;
        stream.close();
        ASSERT_TRUE(stream);
        ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
    };
    write_private(psk_path, "first-declarative-secret\n");
    write_private(config_path, R"json({
        "format_version": 1,
        "servers": [{
            "name": "credential-edge",
            "endpoint": "edge.example.com:2333",
            "psk_file": "edge.psk",
            "enabled": true
        }],
        "tunnels": []
    })json");

    const auto first =
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(first.ok()) << *first.error();
    EXPECT_EQ(require_result(first).at("changed"), 1U);
    EXPECT_EQ(require_result(first).dump().find("first-declarative-secret"), std::string::npos);
    auto server = repository_->servers().get_by_name("credential-edge");
    ASSERT_TRUE(server) << server.error();
    ASSERT_TRUE(server->credential_ref.has_value());
    const std::string first_reference = *server->credential_ref;
    const std::uint64_t first_revision = server->config_revision;
    auto first_secret = credentials_->get(first_reference);
    ASSERT_TRUE(first_secret) << first_secret.error();
    EXPECT_EQ(first_secret->view(), "first-declarative-secret");

    const auto repeated =
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(repeated.ok()) << *repeated.error();
    EXPECT_EQ(require_result(repeated).at("changed"), 0U);
    auto unchanged = repository_->servers().get_by_name("credential-edge");
    ASSERT_TRUE(unchanged) << unchanged.error();
    EXPECT_EQ(unchanged->credential_ref, server->credential_ref);
    EXPECT_EQ(unchanged->config_revision, first_revision);

    write_private(psk_path, "rotated-declarative-secret\n");
    const auto rotated =
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", config_path.string()}});
    ASSERT_TRUE(rotated.ok()) << *rotated.error();
    EXPECT_EQ(require_result(rotated).at("changed"), 1U);
    EXPECT_EQ(require_result(rotated).dump().find("rotated-declarative-secret"), std::string::npos);
    auto updated = repository_->servers().get_by_name("credential-edge");
    ASSERT_TRUE(updated) << updated.error();
    ASSERT_TRUE(updated->credential_ref.has_value());
    EXPECT_NE(*updated->credential_ref, first_reference);
    EXPECT_EQ(updated->config_revision, first_revision + 1U);
    auto old_secret = credentials_->get(first_reference);
    EXPECT_FALSE(old_secret);
    EXPECT_EQ(old_secret.error().code(), common::ErrorCode::not_found);
    auto rotated_secret = credentials_->get(*updated->credential_ref);
    ASSERT_TRUE(rotated_secret) << rotated_secret.error();
    EXPECT_EQ(rotated_secret->view(), "rotated-declarative-secret");

    const auto exported = dispatch(dispatcher_, "config.export", ipc::Json::object());
    ASSERT_TRUE(exported.ok()) << *exported.error();
    const auto export_text = require_result(exported).dump();
    EXPECT_EQ(export_text.find(psk_path.string()), std::string::npos);
    EXPECT_EQ(export_text.find("declarative-secret"), std::string::npos);
    ASSERT_EQ(require_result(exported).at("servers").size(), 1U);
    EXPECT_TRUE(
        require_result(exported).at("servers").at(0).at("credentials").at("psk").get<bool>());
}

TEST_F(DaemonControlServiceTest, UpdatesAndTogglesServerAndTunnelWithMonotonicRevisions) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const auto& initial_server = require_result(added).at("server");
    const auto server_id = initial_server.at("id").get<std::string>();
    const auto initial_server_revision = initial_server.at("config_revision").get<std::uint64_t>();

    const auto updated = dispatch(dispatcher_, "server.update",
                                  ipc::Json{{"identifier", server_id},
                                            {"name", "edge"},
                                            {"endpoint", "edge.example.com:2443"},
                                            {"tls_server_name", "tunnel.example.com"}});
    ASSERT_TRUE(updated.ok()) << *updated.error();
    EXPECT_TRUE(require_result(updated).at("changed").get<bool>());
    const auto& updated_server = require_result(updated).at("server");
    EXPECT_EQ(updated_server.at("name"), "edge");
    EXPECT_EQ(updated_server.at("endpoint"), "edge.example.com:2443");
    EXPECT_EQ(updated_server.at("tls_server_name"), "tunnel.example.com");
    EXPECT_EQ(updated_server.at("config_revision"), initial_server_revision + 1U);

    const auto notifications_after_update = notifications_.load(std::memory_order_relaxed);
    const auto unchanged =
        dispatch(dispatcher_, "server.update",
                 ipc::Json{{"identifier", server_id}, {"endpoint", "edge.example.com:2443"}});
    ASSERT_TRUE(unchanged.ok()) << *unchanged.error();
    EXPECT_FALSE(require_result(unchanged).at("changed").get<bool>());
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_after_update);

    const auto disabled =
        dispatch(dispatcher_, "server.disable", ipc::Json{{"identifier", server_id}});
    ASSERT_TRUE(disabled.ok()) << *disabled.error();
    EXPECT_EQ(require_result(disabled).at("server").at("desired_state"), "disabled");
    EXPECT_EQ(require_result(disabled).at("server").at("actual_state"), "disabled");
    EXPECT_GT(require_result(disabled).at("server").at("config_revision").get<std::uint64_t>(),
              initial_server_revision);

    const auto disabled_again =
        dispatch(dispatcher_, "server.disable", ipc::Json{{"identifier", server_id}});
    ASSERT_TRUE(disabled_again.ok()) << *disabled_again.error();
    EXPECT_FALSE(require_result(disabled_again).at("changed").get<bool>());

    const auto enabled =
        dispatch(dispatcher_, "server.enable", ipc::Json{{"identifier", server_id}});
    ASSERT_TRUE(enabled.ok()) << *enabled.error();
    EXPECT_EQ(require_result(enabled).at("server").at("desired_state"), "enabled");
    EXPECT_EQ(require_result(enabled).at("server").at("actual_state"), "not_authenticated");

    const auto tunnel_added = dispatch(
        dispatcher_, "tun.add",
        ipc::Json{
            {"server", server_id}, {"local_port", 22}, {"remote_port", 6000}, {"name", "ssh"}});
    ASSERT_TRUE(tunnel_added.ok()) << *tunnel_added.error();
    const auto& initial_tunnel = require_result(tunnel_added).at("tunnel");
    const auto tunnel_id = initial_tunnel.at("id").get<std::string>();
    const auto initial_tunnel_revision = initial_tunnel.at("config_revision").get<std::uint64_t>();

    const auto tunnel_updated = dispatch(dispatcher_, "tun.update",
                                         ipc::Json{{"identifier", tunnel_id},
                                                   {"name", "admin"},
                                                   {"local_host", "localhost"},
                                                   {"local_port", 2222},
                                                   {"remote_port", 6022}});
    ASSERT_TRUE(tunnel_updated.ok()) << *tunnel_updated.error();
    const auto& updated_tunnel = require_result(tunnel_updated).at("tunnel");
    EXPECT_EQ(updated_tunnel.at("name"), "admin");
    EXPECT_EQ(updated_tunnel.at("local_endpoint"), "localhost:2222");
    EXPECT_EQ(updated_tunnel.at("remote_endpoint"), "0.0.0.0:6022");
    EXPECT_EQ(updated_tunnel.at("config_revision"), initial_tunnel_revision + 1U);

    const auto tunnel_disabled =
        dispatch(dispatcher_, "tun.disable", ipc::Json{{"identifier", tunnel_id}});
    ASSERT_TRUE(tunnel_disabled.ok()) << *tunnel_disabled.error();
    EXPECT_EQ(require_result(tunnel_disabled).at("tunnel").at("desired_state"), "disabled");
    EXPECT_EQ(require_result(tunnel_disabled).at("tunnel").at("actual_state"), "disabled");
    const auto tunnel_enabled =
        dispatch(dispatcher_, "tun.enable", ipc::Json{{"identifier", tunnel_id}});
    ASSERT_TRUE(tunnel_enabled.ok()) << *tunnel_enabled.error();
    EXPECT_EQ(require_result(tunnel_enabled).at("tunnel").at("desired_state"), "active");
    EXPECT_EQ(require_result(tunnel_enabled).at("tunnel").at("actual_state"), "pending");

    const auto immutable_server =
        dispatch(dispatcher_, "tun.update",
                 ipc::Json{{"identifier", tunnel_id}, {"server", "another-server"}});
    ASSERT_FALSE(immutable_server.ok());
    ASSERT_NE(immutable_server.error(), nullptr);
    EXPECT_EQ(immutable_server.error()->code(), common::ErrorCode::invalid_argument);
}

TEST_F(DaemonControlServiceTest, LogoutDeletesAuthenticationAndConvergesToUnauthenticated) {
    ASSERT_TRUE(dispatch(dispatcher_, "server.add",
                         ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}})
                    .ok());
    ASSERT_TRUE(dispatch(dispatcher_, "server.login",
                         ipc::Json{{"identifier", "primary"}, {"token", "private-token"}})
                    .ok());
    auto before = repository_->servers().get_by_name("primary");
    ASSERT_TRUE(before) << before.error();
    ASSERT_TRUE(before->credential_ref.has_value());
    const std::string old_key = *before->credential_ref;

    const auto logged_out =
        dispatch(dispatcher_, "server.logout", ipc::Json{{"identifier", "primary"}});
    ASSERT_TRUE(logged_out.ok()) << *logged_out.error();
    EXPECT_TRUE(require_result(logged_out).at("changed").get<bool>());
    EXPECT_FALSE(require_result(logged_out).at("server").at("credential_configured").get<bool>());
    EXPECT_EQ(require_result(logged_out).at("server").at("actual_state"), "not_authenticated");

    const auto removed_secret = credentials_->get(old_key);
    ASSERT_FALSE(removed_secret);
    EXPECT_EQ(removed_secret.error().code(), common::ErrorCode::not_found);
    auto after = repository_->servers().get_by_name("primary");
    ASSERT_TRUE(after) << after.error();
    EXPECT_FALSE(after->credential_ref.has_value());
    EXPECT_GT(after->config_revision, before->config_revision);

    const auto repeated =
        dispatch(dispatcher_, "server.logout", ipc::Json{{"identifier", "primary"}});
    ASSERT_TRUE(repeated.ok()) << *repeated.error();
    EXPECT_FALSE(require_result(repeated).at("changed").get<bool>());
}

TEST_F(DaemonControlServiceTest, RejectsInvalidTlsMaterialBeforeSwitchingCredentialReferences) {
    const auto added = dispatch(dispatcher_, "server.add",
                                ipc::Json{{"endpoint", "example.com:2333"}, {"name", "primary"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    auto server_id = common::Id::parse(
        require_result(added).at("server").at("id").get<std::string>(), common::IdKind::server);
    ASSERT_TRUE(server_id) << server_id.error();
    auto before = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(before) << before.error();

    const auto invalid_ca = dispatch(
        dispatcher_, "server.update",
        ipc::Json{{"identifier", server_id->str()}, {"ca_certificate", "not a PEM certificate"}});
    ASSERT_FALSE(invalid_ca.ok());
    ASSERT_NE(invalid_ca.error(), nullptr);
    EXPECT_EQ(invalid_ca.error()->code(), common::ErrorCode::tls_error);

    const auto incomplete_identity = dispatch(
        dispatcher_, "server.update",
        ipc::Json{{"identifier", server_id->str()}, {"client_certificate", "not a certificate"}});
    ASSERT_FALSE(incomplete_identity.ok());
    ASSERT_NE(incomplete_identity.error(), nullptr);
    EXPECT_EQ(incomplete_identity.error()->code(), common::ErrorCode::invalid_argument);

    auto after = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(after) << after.error();
    EXPECT_EQ(after->config_revision, before->config_revision);
    EXPECT_FALSE(after->ca_credential_ref.has_value());
    EXPECT_FALSE(after->client_certificate_ref.has_value());
    EXPECT_FALSE(after->client_private_key_ref.has_value());
    for (const auto kind :
         {ServerCredentialKind::ca_certificate, ServerCredentialKind::client_certificate,
          ServerCredentialKind::client_private_key}) {
        for (const auto& key : managed_server_credential_keys(*server_id, kind)) {
            const auto missing = credentials_->get(key);
            ASSERT_FALSE(missing);
            EXPECT_EQ(missing.error().code(), common::ErrorCode::not_found);
        }
    }
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

TEST_F(DaemonControlServiceTest, RejectsMalformedParametersAcrossTheCompleteControlSurface) {
    struct InvalidRequest final {
        std::string method;
        ipc::Json params;
    };
    const std::vector<InvalidRequest> no_parameter_methods{
        {"daemon.status", ipc::Json{{"unexpected", true}}},
        {"daemon.identity", ipc::Json{{"unexpected", true}}},
        {"status", ipc::Json{{"unexpected", true}}},
        {"server.list", ipc::Json{{"unexpected", true}}},
        {"config.export", ipc::Json{{"unexpected", true}}},
        {"health", ipc::Json{{"unexpected", true}}},
        {"readiness", ipc::Json{{"unexpected", true}}},
        {"metrics", ipc::Json{{"unexpected", true}}},
        {"reload", ipc::Json{{"unexpected", true}}},
    };
    for (const auto& request : no_parameter_methods) {
        SCOPED_TRACE(request.method);
        expect_control_error(dispatch(dispatcher_, request.method, request.params));
    }

    const std::vector<InvalidRequest> missing_required_fields{
        {"server.add", ipc::Json::object()},
        {"server.login", ipc::Json{{"psk", "secret"}}},
        {"server.update", ipc::Json{{"name", "renamed"}}},
        {"server.enable", ipc::Json::object()},
        {"server.disable", ipc::Json::object()},
        {"server.logout", ipc::Json::object()},
        {"server.inspect", ipc::Json::object()},
        {"server.remove", ipc::Json::object()},
        {"tun.add", ipc::Json{{"server", "edge"}, {"local_port", 22}}},
        {"tun.update", ipc::Json{{"name", "renamed"}}},
        {"tun.enable", ipc::Json::object()},
        {"tun.disable", ipc::Json::object()},
        {"tun.inspect", ipc::Json::object()},
        {"tun.remove", ipc::Json::object()},
        {"config.plan", ipc::Json::object()},
        {"config.apply", ipc::Json::object()},
    };
    for (const auto& request : missing_required_fields) {
        SCOPED_TRACE(request.method);
        expect_control_error(dispatch(dispatcher_, request.method, request.params));
    }

    ASSERT_TRUE(dispatch(dispatcher_, "server.add",
                         ipc::Json{{"endpoint", "edge.example.com:2333"}, {"name", "edge"}})
                    .ok());
    ASSERT_TRUE(dispatch(dispatcher_, "tun.add",
                         ipc::Json{{"server", "edge"},
                                   {"name", "ssh"},
                                   {"local_port", 22},
                                   {"remote_port", 6022}})
                    .ok());

    const std::vector<InvalidRequest> invalid_values{
        {"server.add", ipc::Json{{"endpoint", 5}}},
        {"server.add", ipc::Json{{"endpoint", "valid.example.com:1"}, {"name", false}}},
        {"server.login", ipc::Json{{"identifier", "edge"}}},
        {"server.login", ipc::Json{{"identifier", "edge"}, {"psk", "a"}, {"token", "b"}}},
        {"server.login", ipc::Json{{"identifier", 7}, {"psk", "secret"}}},
        {"server.login", ipc::Json{{"identifier", "edge"}, {"psk", false}}},
        {"server.login", ipc::Json{{"identifier", "edge"}, {"psk", ""}}},
        {"server.login",
         ipc::Json{{"identifier", "edge"},
                   {"psk", std::string(storage::kMaxCredentialSecretBytes + 1U, 'x')}}},
        {"server.update", ipc::Json{{"identifier", "edge"}}},
        {"server.update", ipc::Json{{"identifier", 7}, {"name", "new"}}},
        {"server.update", ipc::Json{{"identifier", "edge"}, {"name", false}}},
        {"server.update", ipc::Json{{"identifier", "edge"}, {"endpoint", nullptr}}},
        {"server.update", ipc::Json{{"identifier", "edge"}, {"tls_server_name", false}}},
        {"server.update", ipc::Json{{"identifier", "edge"}, {"ca_certificate", false}}},
        {"server.update", ipc::Json{{"identifier", "edge"}, {"client_certificate", false}}},
        {"server.update", ipc::Json{{"identifier", "edge"}, {"client_private_key", false}}},
        {"server.update", ipc::Json{{"identifier", "edge"}, {"unknown", true}}},
        {"server.enable", ipc::Json{{"identifier", 1}}},
        {"server.disable", ipc::Json{{"identifier", 1}}},
        {"server.logout", ipc::Json{{"identifier", 1}}},
        {"server.inspect", ipc::Json{{"identifier", 1}}},
        {"server.remove", ipc::Json{{"identifier", 1}}},
        {"tun.add", ipc::Json{{"server", false}, {"local_port", 22}, {"remote_port", 6022}}},
        {"tun.add", ipc::Json{{"server", "edge"}, {"local_port", "22"}, {"remote_port", 6022}}},
        {"tun.add", ipc::Json{{"server", "edge"}, {"local_port", -1}, {"remote_port", 6022}}},
        {"tun.add", ipc::Json{{"server", "edge"}, {"local_port", 65'536}, {"remote_port", 6022}}},
        {"tun.add", ipc::Json{{"server", "edge"}, {"local_port", 22}, {"remote_port", 0}}},
        {"tun.add",
         ipc::Json{
             {"server", "edge"}, {"local_host", false}, {"local_port", 22}, {"remote_port", 6022}}},
        {"tun.add",
         ipc::Json{{"server", "edge"}, {"name", false}, {"local_port", 22}, {"remote_port", 6022}}},
        {"tun.update", ipc::Json{{"identifier", "ssh"}}},
        {"tun.update", ipc::Json{{"identifier", 1}, {"name", "new"}}},
        {"tun.update", ipc::Json{{"identifier", "ssh"}, {"name", false}}},
        {"tun.update", ipc::Json{{"identifier", "ssh"}, {"local_host", nullptr}}},
        {"tun.update", ipc::Json{{"identifier", "ssh"}, {"local_port", "22"}}},
        {"tun.update", ipc::Json{{"identifier", "ssh"}, {"remote_port", -1}}},
        {"tun.update", ipc::Json{{"identifier", "ssh"}, {"remote_port", 65'536}}},
        {"tun.update", ipc::Json{{"identifier", "ssh"}, {"server", "other"}}},
        {"tun.enable", ipc::Json{{"identifier", 1}}},
        {"tun.disable", ipc::Json{{"identifier", 1}}},
        {"tun.inspect", ipc::Json{{"identifier", 1}}},
        {"tun.remove", ipc::Json{{"identifier", 1}}},
        {"tun.list", ipc::Json{{"server", false}}},
        {"config.plan", ipc::Json{{"path", false}}},
        {"config.plan", ipc::Json{{"path", "missing"}, {"prune", "yes"}}},
        {"config.apply", ipc::Json{{"path", false}}},
        {"config.apply", ipc::Json{{"path", "missing"}, {"prune", 1}}},
        {"doctor", ipc::Json{{"restore_state", false}}},
        {"doctor", ipc::Json{{"restore_credentials", false}}},
        {"doctor", ipc::Json{{"unknown", true}}},
    };
    for (const auto& request : invalid_values) {
        SCOPED_TRACE(request.method + ": " + request.params.dump());
        expect_control_error(dispatch(dispatcher_, request.method, request.params));
    }
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

TEST_F(DaemonControlServiceTest, HandlerRegistrationRollsBackOnMethodCollision) {
    ipc::Dispatcher collision;
    ASSERT_TRUE(collision.register_handler("server.update",
                                           [](const ipc::Request&) -> common::Result<ipc::Json> {
                                               return ipc::Json{{"sentinel", true}};
                                           }));
    ControlService service{*repository_, *credentials_};

    const auto registered = service.register_handlers(collision);
    ASSERT_FALSE(registered);
    EXPECT_EQ(registered.error().code(), common::ErrorCode::already_exists);
    EXPECT_EQ(collision.size(), 1U);
    const auto sentinel = dispatch(collision, "server.update", ipc::Json::object());
    ASSERT_TRUE(sentinel.ok());
    EXPECT_TRUE(require_result(sentinel).at("sentinel").get<bool>());
    expect_control_error(dispatch(collision, "daemon.status", ipc::Json::object()),
                         common::ErrorCode::not_found);
}

TEST_F(DaemonControlServiceTest, ContainsCallbacksAndSupportsMissingControlProviders) {
    FailingCredentialStore non_sqlite{*credentials_};
    ipc::Dispatcher missing_dispatcher;
    ControlService missing{*repository_, non_sqlite};
    ASSERT_TRUE(missing.register_handlers(missing_dispatcher));

    const auto status = dispatch(missing_dispatcher, "status", ipc::Json::object());
    ASSERT_TRUE(status.ok()) << *status.error();
    EXPECT_EQ(require_result(status).at("runtime").at("sessions").at("active"), 0U);
    const auto metrics = dispatch(missing_dispatcher, "metrics", ipc::Json::object());
    ASSERT_TRUE(metrics.ok()) << *metrics.error();
    EXPECT_TRUE(require_result(metrics).empty());
    expect_control_error(dispatch(missing_dispatcher, "health", ipc::Json::object()),
                         common::ErrorCode::unsupported_version);
    const auto readiness = dispatch(missing_dispatcher, "readiness", ipc::Json::object());
    ASSERT_TRUE(readiness.ok()) << *readiness.error();
    EXPECT_FALSE(require_result(readiness).at("ready").get<bool>());
    EXPECT_EQ(require_result(readiness).at("reason"), "credential_store_unavailable");
    expect_control_error(dispatch(missing_dispatcher, "doctor", ipc::Json::object()),
                         common::ErrorCode::unsupported_version);
    expect_control_error(dispatch(missing_dispatcher, "reload", ipc::Json::object()),
                         common::ErrorCode::unsupported_version);

    ipc::Dispatcher throwing_dispatcher;
    ControlService throwing{
        *repository_, *credentials_, [] { throw std::runtime_error("state callback failure"); },
        []() -> ipc::Json { throw std::runtime_error("metrics callback failure"); },
        [] {
            return common::Result<void>::failure(common::ErrorCode::internal_error,
                                                 "reload callback failure");
        }};
    ASSERT_TRUE(throwing.register_handlers(throwing_dispatcher));
    const auto added =
        dispatch(throwing_dispatcher, "server.add",
                 ipc::Json{{"endpoint", "callback.example.com:2333"}, {"name", "callback"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const auto degraded_status = dispatch(throwing_dispatcher, "status", ipc::Json::object());
    ASSERT_TRUE(degraded_status.ok()) << *degraded_status.error();
    EXPECT_TRUE(require_result(degraded_status).at("runtime").at("provider_error").get<bool>());
    expect_control_error(dispatch(throwing_dispatcher, "metrics", ipc::Json::object()),
                         common::ErrorCode::internal_error);
    expect_control_error(dispatch(throwing_dispatcher, "reload", ipc::Json::object()),
                         common::ErrorCode::internal_error);

    ipc::Dispatcher non_object_dispatcher;
    ControlService non_object{*repository_, *credentials_, {}, [] { return ipc::Json::array(); }};
    ASSERT_TRUE(non_object.register_handlers(non_object_dispatcher));
    const auto non_object_metrics = dispatch(non_object_dispatcher, "metrics", ipc::Json::object());
    ASSERT_TRUE(non_object_metrics.ok()) << *non_object_metrics.error();
    EXPECT_TRUE(require_result(non_object_metrics).empty());
    const auto non_object_status = dispatch(non_object_dispatcher, "status", ipc::Json::object());
    ASSERT_TRUE(non_object_status.ok()) << *non_object_status.error();
    EXPECT_FALSE(require_result(non_object_status).at("runtime").contains("provider_error"));
}

TEST_F(DaemonControlServiceTest, ReportsEveryPendingReasonAndAmbiguousTunnelName) {
    const auto first_server =
        dispatch(dispatcher_, "server.add",
                 ipc::Json{{"endpoint", "first.example.com:2333"}, {"name", "first"}});
    ASSERT_TRUE(first_server.ok()) << *first_server.error();
    const auto second_server =
        dispatch(dispatcher_, "server.add",
                 ipc::Json{{"endpoint", "second.example.com:2333"}, {"name", "second"}});
    ASSERT_TRUE(second_server.ok()) << *second_server.error();
    const auto first_tunnel = dispatch(
        dispatcher_, "tun.add",
        ipc::Json{
            {"server", "first"}, {"name", "shared"}, {"local_port", 22}, {"remote_port", 6'100}});
    ASSERT_TRUE(first_tunnel.ok()) << *first_tunnel.error();
    ASSERT_TRUE(dispatch(dispatcher_, "tun.add",
                         ipc::Json{{"server", "second"},
                                   {"name", "shared"},
                                   {"local_port", 23},
                                   {"remote_port", 6'100}})
                    .ok());
    expect_control_error(dispatch(dispatcher_, "tun.inspect", ipc::Json{{"identifier", "shared"}}));
    expect_control_error(dispatch(dispatcher_, "tun.inspect", ipc::Json{{"identifier", ""}}));
    expect_control_error(
        dispatch(dispatcher_, "tun.inspect",
                 ipc::Json{{"identifier", std::string(storage::kMaxNameBytes + 1U, 'n')}}));

    const std::string server_id_text =
        require_result(first_server).at("server").at("id").get<std::string>();
    const std::string tunnel_id_text =
        require_result(first_tunnel).at("tunnel").at("id").get<std::string>();
    auto server_id = common::Id::parse(server_id_text, common::IdKind::server);
    auto tunnel_id = common::Id::parse(tunnel_id_text, common::IdKind::tunnel);
    ASSERT_TRUE(server_id) << server_id.error();
    ASSERT_TRUE(tunnel_id) << tunnel_id.error();
    auto server = repository_->servers().get_by_id(*server_id);
    ASSERT_TRUE(server) << server.error();

    struct StateReason final {
        storage::ServerActualState state;
        std::string_view reason;
    };
    constexpr StateReason reasons[]{
        {storage::ServerActualState::not_authenticated, "server_not_authenticated"},
        {storage::ServerActualState::disconnected, "server_disconnected"},
        {storage::ServerActualState::connecting, "server_connecting"},
        {storage::ServerActualState::tls_handshake, "server_tls_handshake"},
        {storage::ServerActualState::authenticating, "server_authenticating"},
        {storage::ServerActualState::online, "awaiting_remote_sync"},
        {storage::ServerActualState::backoff, "server_backoff"},
        {storage::ServerActualState::disabled, "server_disabled"},
        {storage::ServerActualState::error, "server_error"},
    };
    for (const auto& expected : reasons) {
        server->actual_state = expected.state;
        ++server->updated_at_unix_ms;
        ASSERT_TRUE(repository_->servers().update(*server));
        const auto inspected =
            dispatch(dispatcher_, "tun.inspect", ipc::Json{{"identifier", tunnel_id_text}});
        ASSERT_TRUE(inspected.ok()) << *inspected.error();
        EXPECT_EQ(require_result(inspected).at("tunnel").at("pending_reason"), expected.reason);
    }

    auto tunnel = repository_->tunnels().get_by_id(*tunnel_id);
    ASSERT_TRUE(tunnel) << tunnel.error();
    tunnel->actual_state = storage::TunnelActualState::active;
    ++tunnel->updated_at_unix_ms;
    ASSERT_TRUE(repository_->tunnels().update(*tunnel));
    const auto active =
        dispatch(dispatcher_, "tun.inspect", ipc::Json{{"identifier", tunnel_id_text}});
    ASSERT_TRUE(active.ok()) << *active.error();
    EXPECT_TRUE(require_result(active).at("tunnel").at("pending_reason").is_null());

    tunnel->actual_state = storage::TunnelActualState::pending;
    ++tunnel->updated_at_unix_ms;
    ASSERT_TRUE(repository_->tunnels().update(*tunnel));
    auto orphan_server = common::Id::generate(common::IdKind::server);
    ASSERT_TRUE(orphan_server) << orphan_server.error();
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("PRAGMA foreign_keys = OFF");
        native.execute("UPDATE tunnels SET server_id = '" + orphan_server->str() +
                       "' WHERE id = '" + tunnel_id_text + "'");
    }
    const auto orphan = dispatch(dispatcher_, "tun.list", ipc::Json::object());
    ASSERT_TRUE(orphan.ok()) << *orphan.error();
    const auto orphan_tunnel = std::find_if(
        require_result(orphan).at("tunnels").begin(), require_result(orphan).at("tunnels").end(),
        [&tunnel_id_text](const ipc::Json& item) { return item.at("id") == tunnel_id_text; });
    ASSERT_NE(orphan_tunnel, require_result(orphan).at("tunnels").end());
    EXPECT_EQ(orphan_tunnel->at("pending_reason"), "server_not_found");
    EXPECT_TRUE(orphan_tunnel->at("server_name").is_null());
}

TEST_F(DaemonControlServiceTest, RejectsExhaustedConfigurationRevisionsAndRemovedTombstones) {
    const auto added =
        dispatch(dispatcher_, "server.add",
                 ipc::Json{{"endpoint", "revision.example.com:2333"}, {"name", "revision"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const std::string server_id = require_result(added).at("server").at("id").get<std::string>();
    const auto tunnel = dispatch(dispatcher_, "tun.add",
                                 ipc::Json{{"server", "revision"},
                                           {"name", "revision-tunnel"},
                                           {"local_port", 22},
                                           {"remote_port", 6'200}});
    ASSERT_TRUE(tunnel.ok()) << *tunnel.error();
    const std::string tunnel_id = require_result(tunnel).at("tunnel").at("id").get<std::string>();
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("UPDATE servers SET config_revision = 9223372036854775807 WHERE id = '" +
                       server_id + "'");
        native.execute("UPDATE tunnels SET config_revision = 9223372036854775807 WHERE id = '" +
                       tunnel_id + "'");
    }
    expect_control_error(dispatch(dispatcher_, "server.login",
                                  ipc::Json{{"identifier", server_id}, {"psk", "secret"}}),
                         common::ErrorCode::resource_exhausted);
    expect_control_error(dispatch(dispatcher_, "tun.update",
                                  ipc::Json{{"identifier", tunnel_id}, {"remote_port", 6'201}}),
                         common::ErrorCode::resource_exhausted);

    auto parsed_server = common::Id::parse(server_id, common::IdKind::server);
    auto parsed_tunnel = common::Id::parse(tunnel_id, common::IdKind::tunnel);
    ASSERT_TRUE(parsed_server) << parsed_server.error();
    ASSERT_TRUE(parsed_tunnel) << parsed_tunnel.error();
    ASSERT_TRUE(repository_->tunnels().mark_removed(*parsed_tunnel, 9'223'372'036'854'000LL));
    expect_control_error(dispatch(dispatcher_, "tun.inspect", ipc::Json{{"identifier", tunnel_id}}),
                         common::ErrorCode::not_found);
    ASSERT_TRUE(repository_->servers().mark_removed(*parsed_server, 9'223'372'036'854'001LL));
    expect_control_error(
        dispatch(dispatcher_, "server.inspect", ipc::Json{{"identifier", server_id}}),
        common::ErrorCode::not_found);
    const auto status = dispatch(dispatcher_, "status", ipc::Json::object());
    ASSERT_TRUE(status.ok()) << *status.error();
    EXPECT_EQ(require_result(status).at("servers").at("total"), 0U);
    EXPECT_EQ(require_result(status).at("tunnels").at("total"), 0U);
}

TEST_F(DaemonControlServiceTest, DoctorBacksUpCheckpointsAndRestoresBothDatabases) {
    const auto state_backup = state_file_.directory() / "doctor-state.db";
    const auto credential_backup = credential_file_.directory() / "doctor-credentials.db";
    const auto backed_up = dispatch(dispatcher_, "doctor",
                                    ipc::Json{{"backup_state", state_backup.string()},
                                              {"backup_credentials", credential_backup.string()},
                                              {"checkpoint", true}});
    ASSERT_TRUE(backed_up.ok()) << *backed_up.error();
    EXPECT_EQ(require_result(backed_up).at("actions").at("backup_state"), state_backup.string());
    EXPECT_EQ(require_result(backed_up).at("actions").at("backup_credentials"),
              credential_backup.string());

    ASSERT_TRUE(dispatch(dispatcher_, "server.add",
                         ipc::Json{{"endpoint", "restore.example.com:2333"}, {"name", "restore"}})
                    .ok());
    ASSERT_TRUE(dispatch(dispatcher_, "server.login",
                         ipc::Json{{"identifier", "restore"}, {"psk", "restore-secret"}})
                    .ok());
    const auto notifications_before_restore = notifications_.load(std::memory_order_relaxed);
    const auto restored = dispatch(dispatcher_, "doctor",
                                   ipc::Json{{"restore_state", state_backup.string()},
                                             {"restore_credentials", credential_backup.string()}});
    ASSERT_TRUE(restored.ok()) << *restored.error();
    EXPECT_EQ(require_result(restored).at("actions").at("restore_consistency"),
              "prevalidated_per_database");
    EXPECT_GT(notifications_.load(std::memory_order_relaxed), notifications_before_restore);
    const auto servers = dispatch(dispatcher_, "server.list", ipc::Json::object());
    ASSERT_TRUE(servers.ok()) << *servers.error();
    EXPECT_TRUE(require_result(servers).at("servers").empty());

    const auto state_only =
        dispatch(dispatcher_, "doctor", ipc::Json{{"restore_state", state_backup.string()}});
    ASSERT_TRUE(state_only.ok()) << *state_only.error();
    EXPECT_EQ(require_result(state_only).at("actions").at("restore_consistency"),
              "single_database_atomic");

    for (const auto& backup_field : {"backup_state", "backup_credentials"}) {
        const auto refused = dispatch(dispatcher_, "doctor",
                                      ipc::Json{{backup_field,
                                                 backup_field == std::string_view{"backup_state"}
                                                     ? state_file_.directory().string()
                                                     : credential_file_.directory().string()}});
        ASSERT_FALSE(refused.ok());
        EXPECT_TRUE(refused.error()->code() == common::ErrorCode::permission_denied ||
                    refused.error()->code() == common::ErrorCode::already_exists)
            << refused.error()->message();
    }
}

TEST_F(DaemonControlServiceTest, HealthAndReadinessDescribeDatabaseDamageAndMissingFiles) {
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP INDEX idx_servers_reconcile");
    }
    const auto degraded_health = dispatch(dispatcher_, "health", ipc::Json::object());
    ASSERT_TRUE(degraded_health.ok()) << *degraded_health.error();
    EXPECT_EQ(require_result(degraded_health).at("status"), "degraded");
    EXPECT_FALSE(require_result(degraded_health).at("state_db").get<bool>());
    const auto degraded_readiness = dispatch(dispatcher_, "readiness", ipc::Json::object());
    ASSERT_TRUE(degraded_readiness.ok()) << *degraded_readiness.error();
    EXPECT_FALSE(require_result(degraded_readiness).at("ready").get<bool>());
    EXPECT_EQ(require_result(degraded_readiness).at("reason"), "database_check_failed");
    const auto degraded_doctor = dispatch(dispatcher_, "doctor", ipc::Json::object());
    ASSERT_TRUE(degraded_doctor.ok()) << *degraded_doctor.error();
    EXPECT_FALSE(require_result(degraded_doctor).at("ok").get<bool>());

    const auto moved_state = state_file_.directory() / "temporarily-moved-state.db";
    ASSERT_EQ(::rename(state_file_.path().c_str(), moved_state.c_str()), 0);
    const auto state_missing = dispatch(dispatcher_, "readiness", ipc::Json::object());
    ASSERT_TRUE(state_missing.ok()) << *state_missing.error();
    EXPECT_EQ(require_result(state_missing).at("reason"), "state_database_unavailable");
    expect_control_error(dispatch(dispatcher_, "health", ipc::Json::object()),
                         common::ErrorCode::database_error);
    ASSERT_EQ(::rename(moved_state.c_str(), state_file_.path().c_str()), 0);

    const auto moved_credentials =
        credential_file_.directory() / "temporarily-moved-credentials.db";
    ASSERT_EQ(::rename(credential_file_.path().c_str(), moved_credentials.c_str()), 0);
    const auto credentials_missing = dispatch(dispatcher_, "readiness", ipc::Json::object());
    ASSERT_TRUE(credentials_missing.ok()) << *credentials_missing.error();
    EXPECT_EQ(require_result(credentials_missing).at("reason"), "credential_database_unavailable");
    expect_control_error(dispatch(dispatcher_, "doctor", ipc::Json::object()),
                         common::ErrorCode::database_error);
    ASSERT_EQ(::rename(moved_credentials.c_str(), credential_file_.path().c_str()), 0);
}

TEST_F(DaemonControlServiceTest, DatabaseStepFailuresRollBackEveryLifecycleMutation) {
    const auto notifications_before = notifications_.load(std::memory_order_relaxed);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_server_insert BEFORE INSERT ON servers BEGIN "
                       "SELECT RAISE(ABORT, 'injected server insert'); END");
    }
    expect_control_error(
        dispatch(dispatcher_, "server.add",
                 ipc::Json{{"endpoint", "step.example.com:2333"}, {"name", "step"}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_server_insert");
    }

    const auto added =
        dispatch(dispatcher_, "server.add",
                 ipc::Json{{"endpoint", "step.example.com:2333"}, {"name", "step"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const std::string server_id = require_result(added).at("server").at("id").get<std::string>();

    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_server_update BEFORE UPDATE ON servers BEGIN "
                       "SELECT RAISE(ABORT, 'injected server update'); END");
    }
    expect_control_error(dispatch(dispatcher_, "server.update",
                                  ipc::Json{{"identifier", server_id},
                                            {"name", "renamed"}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 1U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_server_update");
    }

    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_server_login BEFORE UPDATE ON servers BEGIN "
                       "SELECT RAISE(ABORT, 'injected server login'); END");
    }
    const auto failed_login = dispatch(dispatcher_, "server.login",
                                       ipc::Json{{"identifier", server_id}, {"psk", "secret"}});
    expect_control_error(failed_login);
    auto parsed_server = common::Id::parse(server_id, common::IdKind::server);
    ASSERT_TRUE(parsed_server) << parsed_server.error();
    for (const auto& key :
         managed_server_credential_keys(*parsed_server, ServerCredentialKind::psk)) {
        EXPECT_EQ(credentials_->get(key).error().code(), common::ErrorCode::not_found);
    }
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 1U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_server_login");
    }

    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_tunnel_insert BEFORE INSERT ON tunnels BEGIN "
                       "SELECT RAISE(ABORT, 'injected tunnel insert'); END");
    }
    expect_control_error(dispatch(dispatcher_, "tun.add",
                                  ipc::Json{{"server", server_id},
                                            {"name", "step-tunnel"},
                                            {"local_port", 22},
                                            {"remote_port", 6'300}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 1U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_tunnel_insert");
    }

    const auto tunnel_added =
        dispatch(dispatcher_, "tun.add",
                 ipc::Json{{"server", server_id},
                           {"name", "step-tunnel"},
                           {"local_port", 22},
                           {"remote_port", 6'300}});
    ASSERT_TRUE(tunnel_added.ok()) << *tunnel_added.error();
    const std::string tunnel_id =
        require_result(tunnel_added).at("tunnel").at("id").get<std::string>();

    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_tunnel_update BEFORE UPDATE ON tunnels BEGIN "
                       "SELECT RAISE(ABORT, 'injected tunnel update'); END");
    }
    expect_control_error(dispatch(dispatcher_, "tun.update",
                                  ipc::Json{{"identifier", tunnel_id}, {"remote_port", 6'301}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 2U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_tunnel_update");
    }

    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_tunnel_remove BEFORE UPDATE OF desired_state ON "
                       "tunnels BEGIN SELECT RAISE(ABORT, 'injected tunnel remove'); END");
    }
    expect_control_error(dispatch(dispatcher_, "tun.remove",
                                  ipc::Json{{"identifier", tunnel_id}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 2U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_tunnel_remove");
    }

    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_server_remove BEFORE UPDATE OF desired_state ON "
                       "servers BEGIN SELECT RAISE(ABORT, 'injected server remove'); END");
    }
    expect_control_error(dispatch(dispatcher_, "server.remove",
                                  ipc::Json{{"identifier", server_id}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 2U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_server_remove");
    }

    const auto apply_path = state_file_.directory() / "apply-step.json";
    write_test_file(apply_path,
                    R"json({"format_version":1,"servers":[{"name":"apply-step","endpoint":"apply.example.com:2333"}],"tunnels":[]})json");
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_config_apply BEFORE INSERT ON servers BEGIN "
                       "SELECT RAISE(ABORT, 'injected config apply'); END");
    }
    expect_control_error(
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", apply_path.string()}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 2U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_config_apply");
    }

    ASSERT_TRUE(dispatch(dispatcher_, "server.login",
                         ipc::Json{{"identifier", server_id}, {"psk", "secret"}})
                    .ok());
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_server_logout BEFORE UPDATE ON servers BEGIN "
                       "SELECT RAISE(ABORT, 'injected server logout'); END");
    }
    expect_control_error(dispatch(dispatcher_, "server.logout",
                                  ipc::Json{{"identifier", server_id}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 3U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_server_logout");
    }

    ASSERT_TRUE(dispatch(dispatcher_, "server.remove",
                         ipc::Json{{"identifier", server_id}})
                    .ok());
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 4U);
}

TEST_F(DaemonControlServiceTest, MoreDatabaseStepFailuresRollBackEveryLifecycleMutation) {
    const auto notifications_before = notifications_.load(std::memory_order_relaxed);
    const auto added =
        dispatch(dispatcher_, "server.add",
                 ipc::Json{{"endpoint", "more.example.com:2333"}, {"name", "more"}});
    ASSERT_TRUE(added.ok()) << *added.error();
    const std::string server_id = require_result(added).at("server").at("id").get<std::string>();
    ASSERT_TRUE(dispatch(dispatcher_, "server.login",
                         ipc::Json{{"identifier", server_id}, {"psk", "first-secret"}})
                    .ok());
    const auto tunnel_added =
        dispatch(dispatcher_, "tun.add",
                 ipc::Json{{"server", server_id},
                           {"name", "more-tunnel"},
                           {"local_port", 22},
                           {"remote_port", 6'400}});
    ASSERT_TRUE(tunnel_added.ok()) << *tunnel_added.error();
    const std::string tunnel_id =
        require_result(tunnel_added).at("tunnel").at("id").get<std::string>();
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 3U);
    ASSERT_TRUE(dispatch(dispatcher_, "server.disable",
                         ipc::Json{{"identifier", server_id}})
                    .ok());
    ASSERT_TRUE(dispatch(dispatcher_, "tun.disable",
                         ipc::Json{{"identifier", tunnel_id}})
                    .ok());
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 5U);

    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_enable BEFORE UPDATE ON servers BEGIN "
                       "SELECT RAISE(ABORT, 'injected enable'); END");
    }
    expect_control_error(
        dispatch(dispatcher_, "server.enable", ipc::Json{{"identifier", server_id}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 5U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_enable");
        native.execute("CREATE TRIGGER reject_tunnel_enable BEFORE UPDATE ON tunnels BEGIN "
                       "SELECT RAISE(ABORT, 'injected tunnel enable'); END");
    }
    expect_control_error(
        dispatch(dispatcher_, "tun.enable", ipc::Json{{"identifier", tunnel_id}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 5U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_tunnel_enable");
    }

    {
        NativeSqliteDatabase native{credential_file_.path()};
        native.execute("CREATE TRIGGER reject_credential_put BEFORE INSERT ON credentials BEGIN "
                       "SELECT RAISE(ABORT, 'injected credential put'); END");
    }
    expect_control_error(dispatch(dispatcher_, "server.login",
                                  ipc::Json{{"identifier", server_id}, {"psk", "second-secret"}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 5U);
    {
        NativeSqliteDatabase native{credential_file_.path()};
        native.execute("DROP TRIGGER reject_credential_put");
    }

    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_transport_update BEFORE UPDATE ON servers BEGIN "
                       "SELECT RAISE(ABORT, 'injected transport update'); END");
    }
    expect_control_error(dispatch(dispatcher_, "server.update",
                                  ipc::Json{{"identifier", server_id},
                                            {"endpoint", "new-more.example.com:2444"}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 5U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_transport_update");
    }

    const auto ca_path = credential_file_.directory() / "more-ca.pem";
    write_test_file(ca_path, "not a real CA\n");
    expect_control_error(dispatch(dispatcher_, "server.update",
                                  ipc::Json{{"identifier", server_id},
                                            {"ca_certificate", ca_path.string()}}),
                         common::ErrorCode::tls_error);
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 5U);

    const auto update_path = state_file_.directory() / "apply-more-update.json";
    write_test_file(update_path,
                    R"json({"format_version":1,"servers":[{"name":"more","endpoint":"applied-more.example.com:2333"}],"tunnels":[]})json");
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_apply_update BEFORE UPDATE ON servers BEGIN "
                       "SELECT RAISE(ABORT, 'injected apply update'); END");
    }
    expect_control_error(
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", update_path.string()}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 5U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_apply_update");
    }

    const auto tunnel_path = state_file_.directory() / "apply-more-tunnel.json";
    write_test_file(tunnel_path,
                    R"json({"format_version":1,"servers":[{"name":"more","endpoint":"more.example.com:2333"}],"tunnels":[{"name":"applied","server":"more","local_port":23,"remote_port":6401}]})json");
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("CREATE TRIGGER reject_apply_tunnel BEFORE INSERT ON tunnels BEGIN "
                       "SELECT RAISE(ABORT, 'injected apply tunnel'); END");
    }
    expect_control_error(
        dispatch(dispatcher_, "config.apply", ipc::Json{{"path", tunnel_path.string()}}));
    EXPECT_EQ(notifications_.load(std::memory_order_relaxed), notifications_before + 5U);
    {
        NativeSqliteDatabase native{state_file_.path()};
        native.execute("DROP TRIGGER reject_apply_tunnel");
    }
}

} // namespace
} // namespace minitun::daemon
