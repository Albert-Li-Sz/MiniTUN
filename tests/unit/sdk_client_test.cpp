#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <minitun/client.h>
#include <minitun/client.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/result.hpp>
#include <minitun/ipc/dispatcher.hpp>
#include <minitun/ipc/local_server.hpp>
#include <minitun/ipc/protocol.hpp>

namespace minitun::sdk {
namespace {

using common::Error;
using common::ErrorCode;
using common::Result;
using ipc::Json;

constexpr std::string_view kServerId = "srv_00000000000000000000000000000001";
constexpr std::string_view kTunnelId = "tun_00000000000000000000000000000001";
constexpr std::string_view kClientId = "client_00000000000000000000000000000001";

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path());
        std::string pattern = (root / "minitun-sdk-test-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error{"failed to create SDK test directory"};
        }
        path_ = created;
    }

    ~TemporaryDirectory() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] std::string file(const std::string_view name) const {
        return (path_ / name).string();
    }

    [[nodiscard]] std::string write(const std::string_view name, const std::string_view contents,
                                    const mode_t mode) const {
        const auto target = file(name);
        std::ofstream stream{target, std::ios::binary | std::ios::trunc};
        if (!stream || !stream.write(contents.data(), static_cast<std::streamsize>(contents.size()))) {
            throw std::runtime_error{"failed to write SDK test input"};
        }
        stream.close();
        if (::chmod(target.c_str(), mode) != 0) {
            throw std::runtime_error{"failed to set SDK test input mode"};
        }
        return target;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] Json server_record() {
    return Json{{"id", kServerId},
                {"name", "primary"},
                {"endpoint", "127.0.0.1:2333"},
                {"tls_server_name", nullptr},
                {"desired_state", "enabled"},
                {"actual_state", "disconnected"},
                {"config_revision", std::uint64_t{7U}},
                {"credential_configured", true},
                {"ca_configured", false},
                {"client_certificate_configured", false},
                {"managed_by_config", false}};
}

[[nodiscard]] Json tunnel_record() {
    return Json{{"id", kTunnelId},
                {"name", "ssh"},
                {"server_id", kServerId},
                {"local_endpoint", "127.0.0.1:22"},
                {"remote_endpoint", "0.0.0.0:22022"},
                {"desired_state", "enabled"},
                {"actual_state", "pending"},
                {"config_revision", std::uint64_t{9U}},
                {"managed_by_config", true}};
}

[[nodiscard]] Json plan_result() {
    return Json{
        {"actions",
         Json::array({Json{{"action", "create"},
                           {"resource", "server"},
                           {"id", nullptr},
                           {"name", "created"}},
                      Json{{"action", "update"},
                           {"resource", "tunnel"},
                           {"id", kTunnelId},
                           {"name", nullptr}},
                      Json{{"action", "disable"},
                           {"resource", "server"},
                           {"id", kServerId},
                           {"name", "disabled"}},
                      Json{{"action", "delete"},
                           {"resource", "tunnel"},
                           {"id", kTunnelId},
                           {"name", "deleted"}}})},
        {"prune", true}};
}

class FakeDaemon final {
  public:
    FakeDaemon()
        : socket_path_(temporary_.file("minitun.sock")), dispatcher_(std::make_shared<ipc::Dispatcher>()),
          server_(io_context_, dispatcher_, ipc::LocalServerOptions{.socket_path = socket_path_}) {
        constexpr std::array methods{
            "daemon.identity", "status",        "server.add",    "server.login",
            "server.update",   "server.enable", "server.disable", "server.logout",
            "server.remove",   "server.list",   "tun.add",       "tun.update",
            "tun.enable",      "tun.disable",   "tun.remove",    "tun.list",
            "config.plan",     "config.apply",  "health",        "readiness",
        };
        for (const std::string_view method : methods) {
            auto registered = dispatcher_->register_handler(
                std::string{method}, [this](const ipc::Request& request) { return reply(request); });
            if (!registered) {
                throw std::runtime_error{"failed to register fake SDK daemon method"};
            }
        }
        auto started = server_.start();
        if (!started) {
            throw std::runtime_error{"failed to start fake SDK daemon"};
        }
        worker_ = std::jthread([this] { io_context_.run(); });
    }

    ~FakeDaemon() noexcept {
        server_.stop();
        io_context_.stop();
    }

    FakeDaemon(const FakeDaemon&) = delete;
    FakeDaemon& operator=(const FakeDaemon&) = delete;

    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }
    [[nodiscard]] const TemporaryDirectory& temporary() const noexcept { return temporary_; }

    void set_json(std::string method, Json value) {
        std::lock_guard lock{mutex_};
        overrides_.insert_or_assign(std::move(method), std::move(value));
    }

    void set_error(std::string method, const ErrorCode code) {
        std::lock_guard lock{mutex_};
        overrides_.insert_or_assign(std::move(method), Error{code, "controlled SDK error"});
    }

    void clear(std::string_view method) {
        std::lock_guard lock{mutex_};
        const auto existing = overrides_.find(method);
        if (existing != overrides_.end()) {
            overrides_.erase(existing);
        }
    }

  private:
    [[nodiscard]] Result<Json> reply(const ipc::Request& request) {
        {
            std::lock_guard lock{mutex_};
            const auto overridden = overrides_.find(request.method);
            if (overridden != overrides_.end()) {
                if (std::holds_alternative<Json>(overridden->second)) {
                    return std::get<Json>(overridden->second);
                }
                return std::get<Error>(overridden->second);
            }
        }

        if (request.method == "daemon.identity") {
            return Json{{"client_id", kClientId}};
        }
        if (request.method == "status") {
            return Json{{"servers", Json{{"total", 2}, {"online", 1}}},
                        {"tunnels", Json{{"total", 3}, {"active", 2}}},
                        {"runtime",
                         Json{{"sessions", Json{{"active", 1}}},
                              {"workers", Json{{"idle", 4}, {"active", 5}}},
                              {"connections", Json{{"active", 6}}}}}};
        }
        if (request.method == "server.list") {
            Json unnamed = server_record();
            unnamed["name"] = nullptr;
            unnamed["tls_server_name"] = "server.example.test";
            return Json{{"servers", Json::array({server_record(), std::move(unnamed)})}};
        }
        if (request.method.starts_with("server.")) {
            return Json{{"server", server_record()}};
        }
        if (request.method == "tun.list") {
            Json unnamed = tunnel_record();
            unnamed["name"] = nullptr;
            return Json{{"tunnels", Json::array({tunnel_record(), std::move(unnamed)})}};
        }
        if (request.method.starts_with("tun.")) {
            return Json{{"tunnel", tunnel_record()}};
        }
        if (request.method == "config.plan" || request.method == "config.apply") {
            return plan_result();
        }
        if (request.method == "health") {
            return Json{{"status", "ok"}, {"state_db", true}, {"credentials_db", true}};
        }
        if (request.method == "readiness") {
            return Json{{"ready", true}};
        }
        return Error{ErrorCode::not_found, "unhandled fake SDK method"};
    }

    TemporaryDirectory temporary_;
    std::string socket_path_;
    asio::io_context io_context_;
    std::shared_ptr<ipc::Dispatcher> dispatcher_;
    ipc::LocalServer server_;
    std::jthread worker_;
    std::mutex mutex_;
    std::map<std::string, std::variant<Json, Error>, std::less<>> overrides_;
};

void expect_error(const int status, minitun_error* error, const minitun_error_code expected) {
    EXPECT_EQ(status, static_cast<int>(expected));
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, expected);
    ASSERT_NE(error->message, nullptr);
    EXPECT_NE(std::strlen(error->message), 0U);
    minitun_error_free(error);
}

class SdkClientTest : public ::testing::Test {
  protected:
    void SetUp() override {
        minitun_client_options options{sizeof(options), daemon_.socket_path().c_str()};
        minitun_error* error = nullptr;
        ASSERT_EQ(minitun_client_create(&options, &client_, &error), 0);
        ASSERT_NE(client_, nullptr);
        ASSERT_EQ(error, nullptr);
    }

    void TearDown() override {
        minitun_client_destroy(client_);
        client_ = nullptr;
    }

    FakeDaemon daemon_;
    minitun_client* client_{nullptr};
};

TEST_F(SdkClientTest, RoundTripsTheCompleteTypedCControlSurface) {
    EXPECT_EQ(minitun_client_abi_version(), MINITUN_CLIENT_ABI_VERSION);

    minitun_client* default_client = nullptr;
    minitun_error* error = nullptr;
    ASSERT_EQ(minitun_client_create(nullptr, &default_client, &error), 0);
    ASSERT_NE(default_client, nullptr);
    EXPECT_EQ(error, nullptr);
    minitun_client_destroy(default_client);
    minitun_client_destroy(nullptr);
    minitun_error_free(nullptr);

    minitun_identity identity{};
    ASSERT_EQ(minitun_client_identity_get(client_, &identity, &error), 0);
    ASSERT_STREQ(identity.client_id, kClientId.data());
    minitun_identity_free(&identity);
    EXPECT_EQ(identity.client_id, nullptr);
    minitun_identity_free(nullptr);

    minitun_status status{};
    ASSERT_EQ(minitun_client_status_get(client_, &status, &error), 0);
    EXPECT_EQ(status.server_total, 2U);
    EXPECT_EQ(status.server_online, 1U);
    EXPECT_EQ(status.tunnel_total, 3U);
    EXPECT_EQ(status.tunnel_active, 2U);
    EXPECT_EQ(status.sessions_active, 1U);
    EXPECT_EQ(status.workers_idle, 4U);
    EXPECT_EQ(status.workers_active, 5U);
    EXPECT_EQ(status.connections_active, 6U);

    minitun_server_info server{};
    minitun_server_create_request server_create{sizeof(server_create), "127.0.0.1:2333",
                                                 "primary"};
    ASSERT_EQ(minitun_client_server_create(client_, &server_create, &server, &error), 0);
    EXPECT_STREQ(server.id, kServerId.data());
    EXPECT_STREQ(server.name, "primary");
    EXPECT_EQ(server.tls_server_name, nullptr);
    EXPECT_EQ(server.config_revision, 7U);
    EXPECT_EQ(server.credential_configured, 1U);
    minitun_server_info_free(&server);

    server_create.name = nullptr;
    ASSERT_EQ(minitun_client_server_create(client_, &server_create, &server, &error), 0);
    minitun_server_info_free(&server);

    ASSERT_EQ(minitun_client_server_login(client_, "primary", "test-psk", &server, &error), 0);
    minitun_server_info_free(&server);

    const auto ca = daemon_.temporary().write("ca.pem", "test-ca\n", 0644);
    const auto certificate = daemon_.temporary().write("client.pem", "test-cert\n", 0644);
    const auto private_key = daemon_.temporary().write("client.key", "test-key\n", 0600);
    minitun_server_update_request server_update{
        sizeof(server_update),
        "primary",
        MINITUN_SERVER_UPDATE_NAME | MINITUN_SERVER_UPDATE_ENDPOINT |
            MINITUN_SERVER_UPDATE_TLS_SERVER_NAME | MINITUN_SERVER_UPDATE_CA_FILE |
            MINITUN_SERVER_UPDATE_CLIENT_CERT_FILE | MINITUN_SERVER_UPDATE_CLIENT_KEY_FILE,
        "renamed",
        "127.0.0.1:2444",
        "server.example.test",
        ca.c_str(),
        certificate.c_str(),
        private_key.c_str(),
    };
    ASSERT_EQ(minitun_client_server_update(client_, &server_update, &server, &error), 0);
    minitun_server_info_free(&server);

    server_update.field_mask = MINITUN_SERVER_UPDATE_NAME | MINITUN_SERVER_UPDATE_TLS_SERVER_NAME |
                               MINITUN_SERVER_UPDATE_CA_FILE |
                               MINITUN_SERVER_UPDATE_CLIENT_CERT_FILE |
                               MINITUN_SERVER_UPDATE_CLIENT_KEY_FILE;
    server_update.name = nullptr;
    server_update.tls_server_name = nullptr;
    server_update.ca_file = nullptr;
    server_update.client_cert_file = nullptr;
    server_update.client_key_file = nullptr;
    ASSERT_EQ(minitun_client_server_update(client_, &server_update, &server, &error), 0);
    minitun_server_info_free(&server);

    for (const auto action : {MINITUN_SERVER_ENABLE, MINITUN_SERVER_DISABLE,
                              MINITUN_SERVER_LOGOUT, MINITUN_SERVER_REMOVE}) {
        ASSERT_EQ(minitun_client_server_execute(client_, "primary", action, &server, &error), 0);
        minitun_server_info_free(&server);
    }
    ASSERT_EQ(minitun_client_server_execute(client_, "primary", MINITUN_SERVER_ENABLE, nullptr,
                                            &error),
              0);

    minitun_server_list servers{};
    ASSERT_EQ(minitun_client_server_list(client_, &servers, &error), 0);
    ASSERT_EQ(servers.size, 2U);
    EXPECT_EQ(servers.items[1].name, nullptr);
    ASSERT_STREQ(servers.items[1].tls_server_name, "server.example.test");
    minitun_server_list_free(&servers);
    EXPECT_EQ(servers.items, nullptr);
    minitun_server_list_free(nullptr);

    minitun_tunnel_info tunnel{};
    minitun_tunnel_create_request tunnel_create{sizeof(tunnel_create), "primary", "ssh",
                                                 "127.0.0.1", 22U, 22022U};
    ASSERT_EQ(minitun_client_tunnel_create(client_, &tunnel_create, &tunnel, &error), 0);
    EXPECT_STREQ(tunnel.id, kTunnelId.data());
    EXPECT_EQ(tunnel.config_revision, 9U);
    minitun_tunnel_info_free(&tunnel);

    tunnel_create.name = nullptr;
    tunnel_create.local_host = nullptr;
    ASSERT_EQ(minitun_client_tunnel_create(client_, &tunnel_create, &tunnel, &error), 0);
    minitun_tunnel_info_free(&tunnel);

    minitun_tunnel_update_request tunnel_update{
        sizeof(tunnel_update),
        "ssh",
        MINITUN_TUNNEL_UPDATE_NAME | MINITUN_TUNNEL_UPDATE_LOCAL_HOST |
            MINITUN_TUNNEL_UPDATE_LOCAL_PORT | MINITUN_TUNNEL_UPDATE_REMOTE_PORT,
        "shell",
        "localhost",
        2222U,
        22222U,
    };
    ASSERT_EQ(minitun_client_tunnel_update(client_, &tunnel_update, &tunnel, &error), 0);
    minitun_tunnel_info_free(&tunnel);
    tunnel_update.field_mask = MINITUN_TUNNEL_UPDATE_NAME;
    tunnel_update.name = nullptr;
    ASSERT_EQ(minitun_client_tunnel_update(client_, &tunnel_update, &tunnel, &error), 0);
    minitun_tunnel_info_free(&tunnel);

    for (const auto action : {MINITUN_TUNNEL_ENABLE, MINITUN_TUNNEL_DISABLE,
                              MINITUN_TUNNEL_REMOVE}) {
        ASSERT_EQ(minitun_client_tunnel_execute(client_, "ssh", action, &tunnel, &error), 0);
        minitun_tunnel_info_free(&tunnel);
    }
    ASSERT_EQ(minitun_client_tunnel_execute(client_, "ssh", MINITUN_TUNNEL_ENABLE, nullptr,
                                            &error),
              0);

    minitun_tunnel_list tunnels{};
    ASSERT_EQ(minitun_client_tunnel_list(client_, nullptr, &tunnels, &error), 0);
    ASSERT_EQ(tunnels.size, 2U);
    minitun_tunnel_list_free(&tunnels);
    ASSERT_EQ(minitun_client_tunnel_list(client_, "primary", &tunnels, &error), 0);
    minitun_tunnel_list_free(&tunnels);
    minitun_tunnel_list_free(nullptr);

    minitun_config_plan_result plan{};
    ASSERT_EQ(minitun_client_config_plan(client_, "/tmp/config.json", 0U, &plan, &error), 0);
    ASSERT_EQ(plan.size, 4U);
    EXPECT_EQ(plan.actions[0].action, MINITUN_CONFIG_CREATE);
    EXPECT_EQ(plan.actions[0].resource, MINITUN_CONFIG_SERVER);
    EXPECT_EQ(plan.actions[1].action, MINITUN_CONFIG_UPDATE);
    EXPECT_EQ(plan.actions[1].resource, MINITUN_CONFIG_TUNNEL);
    EXPECT_EQ(plan.actions[2].action, MINITUN_CONFIG_DISABLE);
    EXPECT_EQ(plan.actions[3].action, MINITUN_CONFIG_DELETE);
    EXPECT_EQ(plan.prune, 1U);
    minitun_config_plan_result_free(&plan);
    ASSERT_EQ(minitun_client_config_apply(client_, "/tmp/config.json", 1U, &plan, &error), 0);
    minitun_config_plan_result_free(&plan);
    minitun_config_plan_result_free(nullptr);

    minitun_config_snapshot snapshot{};
    ASSERT_EQ(minitun_client_config_export(client_, &snapshot, &error), 0);
    EXPECT_EQ(snapshot.servers.size, 2U);
    EXPECT_EQ(snapshot.tunnels.size, 2U);
    minitun_config_snapshot_free(&snapshot);
    minitun_config_snapshot_free(nullptr);

    minitun_diagnostics diagnostics{};
    ASSERT_EQ(minitun_client_diagnostics_get(client_, &diagnostics, &error), 0);
    EXPECT_EQ(diagnostics.healthy, 1U);
    EXPECT_EQ(diagnostics.ready, 1U);
    EXPECT_EQ(diagnostics.state_database_ok, 1U);
    EXPECT_EQ(diagnostics.credential_database_ok, 1U);
}

TEST_F(SdkClientTest, RejectsInvalidArgumentsActionsAndCredentialFiles) {
    minitun_error* error = nullptr;
    minitun_client* output_client = nullptr;
    minitun_client_options small_options{0U, daemon_.socket_path().c_str()};
    expect_error(minitun_client_create(&small_options, &output_client, &error), error,
                 MINITUN_ERROR_UNSUPPORTED_VERSION);
    EXPECT_EQ(output_client, nullptr);
    expect_error(minitun_client_create(nullptr, nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    minitun_client_options empty_path{sizeof(empty_path), ""};
    expect_error(minitun_client_create(&empty_path, &output_client, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    const std::string long_path(4'097U, 'x');
    minitun_client_options oversized_path{sizeof(oversized_path), long_path.c_str()};
    expect_error(minitun_client_create(&oversized_path, &output_client, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    minitun_identity identity{};
    expect_error(minitun_client_identity_get(client_, nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_identity_get(nullptr, &identity, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    minitun_status status{};
    expect_error(minitun_client_status_get(client_, nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_status_get(nullptr, &status, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    minitun_server_info server{};
    minitun_server_create_request server_create{sizeof(server_create), nullptr, nullptr};
    expect_error(minitun_client_server_create(client_, nullptr, &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_server_create(client_, &server_create, &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    server_create.endpoint = "127.0.0.1:1";
    server_create.struct_size = 0U;
    expect_error(minitun_client_server_create(client_, &server_create, &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    server_create.struct_size = sizeof(server_create);
    expect_error(minitun_client_server_create(client_, &server_create, nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    server_create.name = "";
    expect_error(minitun_client_server_create(client_, &server_create, &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    expect_error(minitun_client_server_login(client_, "primary", "secret", nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_server_login(client_, nullptr, "secret", &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_server_login(client_, "primary", nullptr, &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    minitun_server_update_request server_update{sizeof(server_update), "primary", 0U, nullptr,
                                                 nullptr, nullptr, nullptr, nullptr, nullptr};
    expect_error(minitun_client_server_update(client_, nullptr, &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    server_update.struct_size = 0U;
    expect_error(minitun_client_server_update(client_, &server_update, &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    server_update.struct_size = sizeof(server_update);
    expect_error(minitun_client_server_update(client_, &server_update, nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    server_update.identifier = nullptr;
    expect_error(minitun_client_server_update(client_, &server_update, &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    server_update.identifier = "primary";
    server_update.field_mask = MINITUN_SERVER_UPDATE_ENDPOINT;
    server_update.endpoint = "";
    expect_error(minitun_client_server_update(client_, &server_update, &server, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    const auto empty_file = daemon_.temporary().write("empty", "", 0600);
    const auto loose_file = daemon_.temporary().write("loose", "secret", 0666);
    const auto nul_file = daemon_.temporary().write("nul", std::string_view{"a\0b", 3U}, 0600);
    const auto linked_file = daemon_.temporary().write("linked", "secret", 0600);
    const auto hard_link = daemon_.temporary().file("hard-link");
    std::filesystem::create_hard_link(linked_file, hard_link);
    const auto symbolic_link = daemon_.temporary().file("symbolic-link");
    std::filesystem::create_symlink(linked_file, symbolic_link);
    const std::array unsafe_files{daemon_.temporary().file("missing"), empty_file, loose_file,
                                  nul_file, hard_link, symbolic_link};
    server_update.field_mask = MINITUN_SERVER_UPDATE_CLIENT_KEY_FILE;
    for (const auto& file : unsafe_files) {
        server_update.client_key_file = file.c_str();
        const int result = minitun_client_server_update(client_, &server_update, &server, &error);
        EXPECT_NE(result, 0);
        ASSERT_NE(error, nullptr);
        minitun_error_free(error);
        error = nullptr;
    }

    expect_error(minitun_client_server_execute(client_, nullptr, MINITUN_SERVER_ENABLE, &server,
                                                &error),
                 error, MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_server_execute(
                     client_, "primary", static_cast<minitun_server_action>(0), &server, &error),
                 error, MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_server_list(client_, nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    minitun_tunnel_info tunnel{};
    minitun_tunnel_create_request tunnel_create{sizeof(tunnel_create), "primary", nullptr, nullptr,
                                                 0U, 1U};
    expect_error(minitun_client_tunnel_create(client_, &tunnel_create, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    tunnel_create.local_port = 1U;
    tunnel_create.remote_port = 0U;
    expect_error(minitun_client_tunnel_create(client_, &tunnel_create, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    tunnel_create.remote_port = 2U;
    tunnel_create.server = nullptr;
    expect_error(minitun_client_tunnel_create(client_, &tunnel_create, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    tunnel_create.server = "primary";
    tunnel_create.name = "";
    expect_error(minitun_client_tunnel_create(client_, &tunnel_create, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    tunnel_create.name = nullptr;
    tunnel_create.local_host = "";
    expect_error(minitun_client_tunnel_create(client_, &tunnel_create, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    minitun_tunnel_update_request tunnel_update{sizeof(tunnel_update), "ssh", 0U, nullptr, nullptr,
                                                 0U, 0U};
    expect_error(minitun_client_tunnel_update(client_, nullptr, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    tunnel_update.struct_size = 0U;
    expect_error(minitun_client_tunnel_update(client_, &tunnel_update, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    tunnel_update.struct_size = sizeof(tunnel_update);
    tunnel_update.identifier = nullptr;
    expect_error(minitun_client_tunnel_update(client_, &tunnel_update, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    tunnel_update.identifier = "ssh";
    tunnel_update.field_mask = MINITUN_TUNNEL_UPDATE_LOCAL_HOST;
    expect_error(minitun_client_tunnel_update(client_, &tunnel_update, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    tunnel_update.field_mask = MINITUN_TUNNEL_UPDATE_LOCAL_PORT;
    expect_error(minitun_client_tunnel_update(client_, &tunnel_update, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    tunnel_update.field_mask = MINITUN_TUNNEL_UPDATE_REMOTE_PORT;
    expect_error(minitun_client_tunnel_update(client_, &tunnel_update, &tunnel, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_tunnel_execute(
                     client_, "ssh", static_cast<minitun_tunnel_action>(0), &tunnel, &error),
                 error, MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_tunnel_list(client_, "", nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    minitun_config_plan_result plan{};
    expect_error(minitun_client_config_plan(client_, nullptr, 0U, &plan, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_config_plan(client_, "/tmp/config", 0U, nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    expect_error(minitun_client_config_apply(client_, nullptr, 0U, &plan, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    minitun_config_snapshot snapshot{};
    expect_error(minitun_client_config_export(client_, nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);
    minitun_diagnostics diagnostics{};
    expect_error(minitun_client_diagnostics_get(client_, nullptr, &error), error,
                 MINITUN_ERROR_INVALID_ARGUMENT);

    minitun_server_info_free(nullptr);
    minitun_tunnel_info_free(nullptr);
    static_cast<void>(snapshot);
    static_cast<void>(diagnostics);
}

TEST_F(SdkClientTest, MapsDaemonErrorsAndRejectsMalformedTypedResults) {
    struct ErrorMapping final {
        ErrorCode internal;
        minitun_error_code external;
    };
    constexpr std::array mappings{
        ErrorMapping{ErrorCode::invalid_argument, MINITUN_ERROR_INVALID_ARGUMENT},
        ErrorMapping{ErrorCode::not_found, MINITUN_ERROR_NOT_FOUND},
        ErrorMapping{ErrorCode::already_exists, MINITUN_ERROR_ALREADY_EXISTS},
        ErrorMapping{ErrorCode::permission_denied, MINITUN_ERROR_PERMISSION_DENIED},
        ErrorMapping{ErrorCode::not_authenticated, MINITUN_ERROR_NOT_AUTHENTICATED},
        ErrorMapping{ErrorCode::authentication_failed, MINITUN_ERROR_AUTHENTICATION_FAILED},
        ErrorMapping{ErrorCode::connection_failed, MINITUN_ERROR_CONNECTION_FAILED},
        ErrorMapping{ErrorCode::connection_timeout, MINITUN_ERROR_CONNECTION_TIMEOUT},
        ErrorMapping{ErrorCode::remote_port_in_use, MINITUN_ERROR_REMOTE_PORT_IN_USE},
        ErrorMapping{ErrorCode::local_connect_failed, MINITUN_ERROR_LOCAL_CONNECT_FAILED},
        ErrorMapping{ErrorCode::protocol_error, MINITUN_ERROR_PROTOCOL},
        ErrorMapping{ErrorCode::frame_too_large, MINITUN_ERROR_PROTOCOL},
        ErrorMapping{ErrorCode::unsupported_version, MINITUN_ERROR_UNSUPPORTED_VERSION},
        ErrorMapping{ErrorCode::resource_exhausted, MINITUN_ERROR_RESOURCE_EXHAUSTED},
        ErrorMapping{ErrorCode::database_error, MINITUN_ERROR_DATABASE},
        ErrorMapping{ErrorCode::tls_error, MINITUN_ERROR_TLS},
        ErrorMapping{ErrorCode::ipc_error, MINITUN_ERROR_IPC},
        ErrorMapping{ErrorCode::internal_error, MINITUN_ERROR_INTERNAL},
    };
    for (const auto& mapping : mappings) {
        daemon_.set_error("daemon.identity", mapping.internal);
        minitun_identity identity{};
        minitun_error* error = nullptr;
        expect_error(minitun_client_identity_get(client_, &identity, &error), error,
                     mapping.external);
    }
    daemon_.clear("daemon.identity");

    minitun_error* error = nullptr;
    minitun_identity identity{};
    daemon_.set_json("daemon.identity", Json{{"client_id", 7}});
    expect_error(minitun_client_identity_get(client_, &identity, &error), error,
                 MINITUN_ERROR_PROTOCOL);
    daemon_.clear("daemon.identity");

    minitun_status status{};
    daemon_.set_json("status", Json::object());
    expect_error(minitun_client_status_get(client_, &status, &error), error,
                 MINITUN_ERROR_PROTOCOL);
    daemon_.set_json(
        "status",
        Json{{"servers", Json{{"total", -1}, {"online", 1}}},
             {"tunnels", Json{{"total", 1}, {"active", 1}}},
             {"runtime", Json{{"sessions", Json{{"active", 1}}},
                              {"workers", Json{{"idle", 1}, {"active", 1}}},
                              {"connections", Json{{"active", 1}}}}}});
    expect_error(minitun_client_status_get(client_, &status, &error), error,
                 MINITUN_ERROR_PROTOCOL);
    daemon_.clear("status");

    minitun_server_info server{};
    minitun_server_create_request create{sizeof(create), "127.0.0.1:1", nullptr};
    const std::array server_fields{"id",          "endpoint", "desired_state",
                                   "actual_state", "config_revision",
                                   "credential_configured", "ca_configured",
                                   "client_certificate_configured", "managed_by_config"};
    for (const std::string_view field : server_fields) {
        Json invalid = server_record();
        invalid.erase(field);
        daemon_.set_json("server.add", Json{{"server", std::move(invalid)}});
        expect_error(minitun_client_server_create(client_, &create, &server, &error), error,
                     MINITUN_ERROR_PROTOCOL);
    }
    for (const std::string_view field : {std::string_view{"name"},
                                         std::string_view{"tls_server_name"}}) {
        Json invalid = server_record();
        invalid[field] = 1;
        daemon_.set_json("server.add", Json{{"server", std::move(invalid)}});
        expect_error(minitun_client_server_create(client_, &create, &server, &error), error,
                     MINITUN_ERROR_PROTOCOL);
    }
    daemon_.set_json("server.list", Json{{"servers", 1}});
    minitun_server_list servers{};
    expect_error(minitun_client_server_list(client_, &servers, &error), error,
                 MINITUN_ERROR_PROTOCOL);
    daemon_.set_json("server.list",
                     Json{{"servers", Json::array({server_record(), Json::object()})}});
    expect_error(minitun_client_server_list(client_, &servers, &error), error,
                 MINITUN_ERROR_PROTOCOL);
    daemon_.clear("server.add");
    daemon_.clear("server.list");

    minitun_tunnel_info tunnel{};
    minitun_tunnel_create_request tunnel_create{sizeof(tunnel_create), "primary", nullptr,
                                                 "127.0.0.1", 1U, 2U};
    const std::array tunnel_fields{"id",           "server_id",      "local_endpoint",
                                   "remote_endpoint", "desired_state", "actual_state",
                                   "config_revision", "managed_by_config"};
    for (const std::string_view field : tunnel_fields) {
        Json invalid = tunnel_record();
        invalid.erase(field);
        daemon_.set_json("tun.add", Json{{"tunnel", std::move(invalid)}});
        expect_error(minitun_client_tunnel_create(client_, &tunnel_create, &tunnel, &error), error,
                     MINITUN_ERROR_PROTOCOL);
    }
    Json invalid_tunnel = tunnel_record();
    invalid_tunnel["name"] = false;
    daemon_.set_json("tun.add", Json{{"tunnel", std::move(invalid_tunnel)}});
    expect_error(minitun_client_tunnel_create(client_, &tunnel_create, &tunnel, &error), error,
                 MINITUN_ERROR_PROTOCOL);
    daemon_.set_json("tun.list", Json{{"tunnels", false}});
    minitun_tunnel_list tunnels{};
    expect_error(minitun_client_tunnel_list(client_, nullptr, &tunnels, &error), error,
                 MINITUN_ERROR_PROTOCOL);
    daemon_.set_json("tun.list",
                     Json{{"tunnels", Json::array({tunnel_record(), Json::object()})}});
    expect_error(minitun_client_tunnel_list(client_, nullptr, &tunnels, &error), error,
                 MINITUN_ERROR_PROTOCOL);
    daemon_.clear("tun.add");
    daemon_.clear("tun.list");

    minitun_config_plan_result plan{};
    for (const Json& invalid :
         {Json::object(), Json{{"actions", 1}, {"prune", true}},
          Json{{"actions", Json::array()}, {"prune", 1}},
          Json{{"actions", Json::array({1})}, {"prune", false}},
          Json{{"actions", Json::array({Json{{"resource", "server"}}})}, {"prune", false}},
          Json{{"actions", Json::array({Json{{"action", "unknown"},
                                             {"resource", "server"}}})},
               {"prune", false}},
          Json{{"actions", Json::array({Json{{"action", "create"},
                                             {"resource", "unknown"}}})},
               {"prune", false}},
          Json{{"actions", Json::array({Json{{"action", "create"},
                                             {"resource", "server"},
                                             {"id", 1},
                                             {"name", nullptr}}})},
               {"prune", false}}}) {
        daemon_.set_json("config.plan", invalid);
        expect_error(minitun_client_config_plan(client_, "/tmp/config", 0U, &plan, &error), error,
                     MINITUN_ERROR_PROTOCOL);
    }
    daemon_.clear("config.plan");

    minitun_config_snapshot snapshot{};
    daemon_.set_error("server.list", ErrorCode::database_error);
    expect_error(minitun_client_config_export(client_, &snapshot, &error), error,
                 MINITUN_ERROR_INTERNAL);
    daemon_.clear("server.list");
    daemon_.set_error("tun.list", ErrorCode::database_error);
    expect_error(minitun_client_config_export(client_, &snapshot, &error), error,
                 MINITUN_ERROR_INTERNAL);
    daemon_.clear("tun.list");

    minitun_diagnostics diagnostics{};
    daemon_.set_error("health", ErrorCode::database_error);
    expect_error(minitun_client_diagnostics_get(client_, &diagnostics, &error), error,
                 MINITUN_ERROR_DATABASE);
    daemon_.clear("health");
    daemon_.set_error("readiness", ErrorCode::ipc_error);
    expect_error(minitun_client_diagnostics_get(client_, &diagnostics, &error), error,
                 MINITUN_ERROR_IPC);
    daemon_.clear("readiness");
    daemon_.set_json("health", Json{{"status", 1}, {"state_db", true},
                                     {"credentials_db", true}});
    expect_error(minitun_client_diagnostics_get(client_, &diagnostics, &error), error,
                 MINITUN_ERROR_PROTOCOL);
}

TEST_F(SdkClientTest, CppWrapperOwnsResultsAndCoversEveryAction) {
    auto created = Client::create(daemon_.socket_path());
    ASSERT_TRUE(created) << created.error().message;
    Client client = std::move(created).value();

    auto identity = client.identity();
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity.value().client_id, kClientId);
    auto status = client.status();
    ASSERT_TRUE(status);
    EXPECT_EQ(status.value().tunnel_active, 2U);

    ASSERT_TRUE(client.create_server("127.0.0.1:2333", "primary"));
    ASSERT_TRUE(client.login_server("primary", "test-psk"));
    ServerUpdate server_update;
    server_update.identifier = "primary";
    server_update.name = UpdateField<std::string>::set("renamed");
    server_update.endpoint = UpdateField<std::string>::set("127.0.0.1:2444");
    server_update.tls_server_name = UpdateField<std::string>::clear();
    server_update.ca_file = UpdateField<std::string>::clear();
    server_update.client_certificate_file = UpdateField<std::string>::clear();
    server_update.client_private_key_file = UpdateField<std::string>::clear();
    ASSERT_TRUE(client.update_server(server_update));
    for (const auto action : {ServerAction::enable, ServerAction::disable, ServerAction::logout,
                              ServerAction::remove}) {
        auto result = client.execute_server("primary", action);
        ASSERT_TRUE(result);
        if (action == ServerAction::remove) {
            EXPECT_EQ(result.value().desired_state, "removed");
        }
    }
    auto servers = client.servers();
    ASSERT_TRUE(servers);
    EXPECT_EQ(servers.value().size(), 2U);

    TunnelCreate tunnel_create{"primary", "ssh", "127.0.0.1", 22U, 22022U};
    ASSERT_TRUE(client.create_tunnel(tunnel_create));
    TunnelUpdate tunnel_update;
    tunnel_update.identifier = "ssh";
    tunnel_update.name = UpdateField<std::string>::clear();
    tunnel_update.local_host = UpdateField<std::string>::set("localhost");
    tunnel_update.local_port = UpdateField<std::uint16_t>::set(2222U);
    tunnel_update.remote_port = UpdateField<std::uint16_t>::set(22222U);
    ASSERT_TRUE(client.update_tunnel(tunnel_update));
    for (const auto action :
         {TunnelAction::enable, TunnelAction::disable, TunnelAction::remove}) {
        ASSERT_TRUE(client.execute_tunnel("ssh", action));
    }
    ASSERT_TRUE(client.tunnels());
    ASSERT_TRUE(client.tunnels(std::string{"primary"}));

    auto plan = client.config_plan("/tmp/config", false);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan.value().actions.size(), 4U);
    EXPECT_EQ(plan.value().actions[0].action, ConfigActionKind::create);
    EXPECT_EQ(plan.value().actions[1].action, ConfigActionKind::update);
    EXPECT_EQ(plan.value().actions[2].action, ConfigActionKind::disable);
    EXPECT_EQ(plan.value().actions[3].action, ConfigActionKind::remove);
    ASSERT_TRUE(client.config_apply("/tmp/config", true));
    auto snapshot = client.config_export();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().servers.size(), 2U);
    EXPECT_EQ(snapshot.value().tunnels.size(), 2U);
    auto diagnostics = client.diagnostics();
    ASSERT_TRUE(diagnostics);
    EXPECT_TRUE(diagnostics.value().healthy);

    daemon_.set_error("daemon.identity", ErrorCode::not_found);
    auto failure = client.identity();
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().code, ClientErrorCode::not_found);
    EXPECT_FALSE(failure.error().message.empty());
}

TEST_F(SdkClientTest, SupportsConcurrentCallsThroughOneHandle) {
    std::atomic_uint failures{0U};
    std::vector<std::jthread> callers;
    for (std::size_t index = 0U; index < 8U; ++index) {
        callers.emplace_back([this, &failures] {
            for (std::size_t iteration = 0U; iteration < 10U; ++iteration) {
                minitun_status status{};
                minitun_error* error = nullptr;
                if (minitun_client_status_get(client_, &status, &error) != 0 || error != nullptr ||
                    status.server_total != 2U) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
                minitun_error_free(error);
            }
        });
    }
    callers.clear();
    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0U);
}

} // namespace
} // namespace minitun::sdk
