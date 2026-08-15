#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <minitun/admin/server.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/time.hpp>
#include <minitun/server/client_policy.hpp>
#include <minitun/server/policy_management.hpp>

#include "storage_test_support.hpp"

namespace minitun::server {
namespace {

using Json = nlohmann::json;

using admin::ManagementRequest;
using admin::ManagementResponse;

using common::ErrorCode;
using common::Result;

constexpr std::string_view kFirstClientId{"client_00000000000000000000000000000001"};
constexpr std::string_view kSecondClientId{"client_00000000000000000000000000000002"};

void write_private(const std::filesystem::path& path, const std::string_view contents,
                   const mode_t mode) {
    storage::test::write_binary_file(path, contents);
    ASSERT_EQ(::chmod(path.c_str(), mode), 0);
}

[[nodiscard]] std::string policy_json(const bool enabled = true) {
    return std::string{"{"} + R"("format_version":1,"clients":[{"client_id":")" +
           std::string{kFirstClientId} + R"(","enabled":)" + (enabled ? "true" : "false") +
           R"(,"psk_file":"client.psk","allowed_ports":["6000","7000-7099"],)" +
           R"("max_tunnels":10,"max_connections":20,"max_idle_workers":30}]})";
}

struct Fixture final {
    Fixture() {
        write_private(temporary.directory() / "client.psk", "policy-secret\n", 0600);
        write_private(temporary.directory() / "clients.json", policy_json(), 0640);
        auto opened =
            ClientPolicyStore::open((temporary.directory() / "clients.json").string(), limits());
        if (!opened) {
            throw std::runtime_error(std::string{"store: "} + opened.error().message());
        }
        store = std::move(*opened);
        bindings = PolicyManagementBindings{
            .document = [store = store] { return store->document(); },
            .replace = [store = store](std::string document) {
                return store->replace(std::move(document));
            },
            .reload = [store = store] { return store->reload(); },
            .server_id = [] { return std::string{"srv_00000000000000000000000000000001"}; },
            .config_directory = [this] { return temporary.directory().string(); },
        };
    }

    [[nodiscard]] static ClientPolicyLimits limits() {
        return ClientPolicyLimits{
            .max_clients = 1'000U,
            .max_tunnels_per_client = 128U,
            .max_connections_per_client = 10'000U,
            .max_idle_workers_per_client = 32U,
        };
    }

    [[nodiscard]] Result<ManagementResponse> call(const std::string_view method,
                                                  const std::string_view path,
                                                  const std::string_view body = {}) const {
        auto handler = make_policy_management_handler(bindings, PolicyManagementOptions{});
        if (!handler) {
            return common::Result<ManagementResponse>::failure(
                common::ErrorCode::internal_error, "handler unavailable");
        }
        return (*handler)(ManagementRequest{std::string{method}, std::string{path},
                                            std::string{body}});
    }

    storage::test::TemporaryDatabaseFile temporary;
    std::shared_ptr<ClientPolicyStore> store;
    PolicyManagementBindings bindings;
};

TEST(PolicyManagementTest, RejectsInvalidOptionsAndUnknownEndpoints) {
    auto invalid = make_policy_management_handler(PolicyManagementBindings{},
                                                  PolicyManagementOptions{
                                                      .rotation_grace_seconds = 0,
                                                  });
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), ErrorCode::invalid_argument);

    Fixture fixture;
    auto missing = fixture.call("GET", "/v1/missing");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::not_found);

    auto wrong_method = fixture.call("DELETE", "/v1/health");
    ASSERT_FALSE(wrong_method);
    EXPECT_EQ(wrong_method.error().code(), ErrorCode::not_found);

    auto malformed = fixture.call("GET", "/v1/clients/not-an-id");
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code(), ErrorCode::invalid_argument);
}

TEST(PolicyManagementTest, ServesHealthListAndSingleClientSummariesWithoutSecrets) {
    Fixture fixture;
    auto health = fixture.call("GET", "/v1/health");
    ASSERT_TRUE(health) << health.error();
    EXPECT_EQ(health->status, 200U);
    auto health_json = Json::parse(health->body);
    EXPECT_EQ(health_json.at("status"), "ok");
    EXPECT_EQ(health_json.at("server_id"), "srv_00000000000000000000000000000001");

    auto listed = fixture.call("GET", "/v1/clients");
    ASSERT_TRUE(listed) << listed.error();
    auto list_json = Json::parse(listed->body);
    ASSERT_EQ(list_json.at("clients").size(), 1U);
    const auto& summary = list_json.at("clients").at(0);
    EXPECT_EQ(summary.at("client_id"), kFirstClientId);
    EXPECT_EQ(summary.at("psk_file"), "client.psk");
    EXPECT_FALSE(summary.contains("psk"));
    EXPECT_FALSE(summary.at("rotation_active").get<bool>());

    auto single = fixture.call("GET", std::string{"/v1/clients/"} + std::string{kFirstClientId});
    ASSERT_TRUE(single) << single.error();
    auto single_json = Json::parse(single->body);
    EXPECT_EQ(single_json.at("client").at("client_id"), kFirstClientId);

    auto absent = fixture.call("GET", std::string{"/v1/clients/"} + std::string{kSecondClientId});
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().code(), ErrorCode::not_found);
}

TEST(PolicyManagementTest, CreatesUpdatesAndDeletesClientPoliciesThroughTheDocument) {
    Fixture fixture;
    const std::string second = std::string{kSecondClientId};
    const std::string create_body =
        R"({"allowed_ports":["8000"],"max_tunnels":5,"max_connections":7,"max_idle_workers":9})";
    auto created = fixture.call("PUT", "/v1/clients/" + second, create_body);
    ASSERT_TRUE(created) << created.error();
    auto created_json = Json::parse(created->body);
    EXPECT_EQ(created_json.at("client").at("client_id"), second);
    EXPECT_TRUE(created_json.at("client").at("enabled").get<bool>());
    ASSERT_TRUE(created_json.contains("psk"));
    EXPECT_EQ(created_json.at("psk").get<std::string>().size(), 64U);
    const std::string generated_psk = created_json.at("psk").get<std::string>();

    // The generated PSK was persisted as a private file and is loadable.
    const std::string generated_path =
        (fixture.temporary.directory() / (second + ".psk")).string();
    struct stat generated_stat {};
    ASSERT_EQ(::stat(generated_path.c_str(), &generated_stat), 0);
    EXPECT_EQ(generated_stat.st_mode & 0077U, 0U);
    EXPECT_EQ(fixture.store->find(second)->psk->view(), generated_psk);

    // Update with an explicit PSK; the response never echoes it back.
    const std::string update_body =
        R"({"enabled":false,"psk":"explicit-replacement-secret","allowed_ports":["8000"],)";
    const std::string full_update =
        update_body + R"("max_tunnels":5,"max_connections":7,"max_idle_workers":9})";
    auto updated = fixture.call("PUT", "/v1/clients/" + second, full_update);
    ASSERT_TRUE(updated) << updated.error();
    auto updated_json = Json::parse(updated->body);
    EXPECT_FALSE(updated_json.contains("psk"));
    EXPECT_FALSE(updated_json.at("client").at("enabled").get<bool>());
    EXPECT_EQ(fixture.store->find(second)->psk->view(), "explicit-replacement-secret");

    auto removed = fixture.call("DELETE", "/v1/clients/" + second);
    ASSERT_TRUE(removed) << removed.error();
    EXPECT_EQ(Json::parse(removed->body).at("removed"), second);
    EXPECT_EQ(fixture.store->find(second), nullptr);

    auto again = fixture.call("DELETE", "/v1/clients/" + second);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code(), ErrorCode::not_found);
}

TEST(PolicyManagementTest, RotatesPsksWithGraceWindowAndAlternatingFiles) {
    Fixture fixture;
    const std::string first = std::string{kFirstClientId};
    const auto now = common::unix_seconds_now();

    auto rotated = fixture.call("POST", "/v1/clients/" + first + "/rotate-psk");
    ASSERT_TRUE(rotated) << rotated.error();
    auto first_json = Json::parse(rotated->body);
    ASSERT_TRUE(first_json.contains("psk"));
    const std::string first_new = first_json.at("psk").get<std::string>();
    EXPECT_EQ(first_new.size(), 64U);
    const std::int64_t first_expiry = first_json.at("previous_psk_expires_at").get<std::int64_t>();
    EXPECT_GE(first_expiry, now + 599);
    EXPECT_LE(first_expiry, now + 601);

    const auto first_policy = fixture.store->find(first);
    ASSERT_NE(first_policy, nullptr);
    EXPECT_EQ(first_policy->psk->view(), first_new);
    ASSERT_NE(first_policy->previous_psk, nullptr);
    EXPECT_EQ(first_policy->previous_psk->view(), "policy-secret");
    EXPECT_EQ(first_policy->rotation_psk(now)->view(), "policy-secret");
    EXPECT_EQ(first_policy->rotation_psk(first_expiry), nullptr);

    auto listed = fixture.call("GET", "/v1/clients");
    auto list_json = Json::parse(listed->body);
    EXPECT_TRUE(list_json.at("clients").at(0).at("rotation_active").get<bool>());

    // A second rotation moves the outgoing secret into the predecessor slot.
    auto second = fixture.call("POST", "/v1/clients/" + first + "/rotate-psk",
                               R"({"grace_seconds":120})");
    ASSERT_TRUE(second) << second.error();
    auto second_json = Json::parse(second->body);
    const std::string second_new = second_json.at("psk").get<std::string>();
    const auto second_policy = fixture.store->find(first);
    ASSERT_NE(second_policy, nullptr);
    EXPECT_EQ(second_policy->psk->view(), second_new);
    ASSERT_NE(second_policy->previous_psk, nullptr);
    EXPECT_EQ(second_policy->previous_psk->view(), first_new);
    EXPECT_NE(second_new, first_new);
    EXPECT_GE(second_json.at("previous_psk_expires_at").get<std::int64_t>(), now + 119);

    // Grace window validation.
    const auto too_long = fixture.call("POST", "/v1/clients/" + first + "/rotate-psk",
                                       R"({"grace_seconds":86401})");
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().code(), ErrorCode::invalid_argument);
    const auto too_short =
        fixture.call("POST", "/v1/clients/" + first + "/rotate-psk", R"({"grace_seconds":0})");
    ASSERT_FALSE(too_short);
    const auto unknown = fixture.call("POST", "/v1/clients/" + first + "/rotate-psk",
                                      R"({"other":1})");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code(), ErrorCode::invalid_argument);
    const auto missing = fixture.call("POST", "/v1/clients/" + std::string{kSecondClientId} +
                                                   "/rotate-psk");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::not_found);
}

TEST(PolicyManagementTest, RejectsMalformedPathsAndUnavailableProviders) {
    // A handler without any bindings rejects document access and mutations.
    auto bare = make_policy_management_handler(PolicyManagementBindings{},
                                               PolicyManagementOptions{});
    ASSERT_TRUE(bare) << bare.error();
    auto no_document = (*bare)(ManagementRequest{"GET", "/v1/clients", ""});
    ASSERT_FALSE(no_document);
    EXPECT_EQ(no_document.error().code(), ErrorCode::internal_error);
    auto no_reload = (*bare)(ManagementRequest{"POST", "/v1/reload", ""});
    ASSERT_FALSE(no_reload);
    EXPECT_EQ(no_reload.error().code(), ErrorCode::internal_error);

    Fixture fixture;
    for (const std::string_view path : {"/v1", "/v1/clients/a/b/c/d", "/v1/clients/a/b/c/d/e"}) {
        SCOPED_TRACE(path);
        auto rejected = fixture.call("GET", path);
        ASSERT_FALSE(rejected) << path;
        EXPECT_EQ(rejected.error().code(), ErrorCode::not_found);
    }
    for (const std::string_view path :
         {"/v1/", "/v1//clients", "/v1/clients/", "v1/clients"}) {
        SCOPED_TRACE(path);
        auto rejected = fixture.call("GET", path);
        ASSERT_FALSE(rejected) << path;
        EXPECT_EQ(rejected.error().code(), ErrorCode::invalid_argument);
    }

    // Methods outside the documented surface are not found.
    const std::string first = std::string{kFirstClientId};
    EXPECT_EQ(fixture.call("DELETE", "/v1/clients").error().code(), ErrorCode::not_found);
    EXPECT_EQ(fixture.call("PATCH", "/v1/clients/" + first).error().code(),
              ErrorCode::not_found);
    EXPECT_EQ(fixture.call("GET", "/v1/clients/" + first + "/rotate-psk").error().code(),
              ErrorCode::not_found);
    EXPECT_EQ(fixture.call("DELETE", "/v1/clients/" + first + "/rotate-psk").error().code(),
              ErrorCode::not_found);
    EXPECT_EQ(fixture.call("POST", "/v1/clients/" + first + "/rotate-psk", "not-json")
                  .error()
                  .code(),
              ErrorCode::invalid_argument);
    EXPECT_EQ(fixture.call("POST", "/v1/clients/" + first + "/rotate-psk", "[]").error().code(),
              ErrorCode::invalid_argument);
    EXPECT_EQ(
        fixture.call("POST", "/v1/clients/" + first + "/rotate-psk", R"({"grace_seconds":"x"})")
            .error()
            .code(),
        ErrorCode::invalid_argument);
    EXPECT_EQ(fixture.call("POST", "/v1/reload", "{}").error().code(),
              ErrorCode::invalid_argument);

    // Reload through a binding that fails propagates the failure.
    PolicyManagementBindings failing = fixture.bindings;
    failing.reload = [] {
        return common::Result<std::vector<std::string>>::failure(
            common::ErrorCode::permission_denied, "reload denied");
    };
    auto failing_handler =
        make_policy_management_handler(failing, PolicyManagementOptions{});
    ASSERT_TRUE(failing_handler) << failing_handler.error();
    auto denied = (*failing_handler)(ManagementRequest{"POST", "/v1/reload", ""});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), ErrorCode::permission_denied);

    // A directory that does not exist surfaces the PSK write failure.
    PolicyManagementBindings bad_directory = fixture.bindings;
    bad_directory.config_directory = [] { return std::string{"/nonexistent/minitun-dir"}; };
    auto bad_handler =
        make_policy_management_handler(bad_directory, PolicyManagementOptions{});
    ASSERT_TRUE(bad_handler) << bad_handler.error();
    const auto created = (*bad_handler)(
        ManagementRequest{"PUT", "/v1/clients/" + std::string{kSecondClientId},
                          R"({"allowed_ports":["8000"],"max_tunnels":5,"max_connections":7,)"
                          R"("max_idle_workers":9})"});
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().code(), ErrorCode::permission_denied);

    // A replace binding that fails surfaces the persistence failure.
    PolicyManagementBindings bad_replace = fixture.bindings;
    bad_replace.replace = [](std::string) {
        return common::Result<std::vector<std::string>>::failure(
            common::ErrorCode::database_error, "write denied");
    };
    auto replace_handler =
        make_policy_management_handler(bad_replace, PolicyManagementOptions{});
    ASSERT_TRUE(replace_handler) << replace_handler.error();
    const auto failed = (*replace_handler)(ManagementRequest{
        "PUT", "/v1/clients/" + std::string{kSecondClientId},
        R"({"psk":"another-secret","allowed_ports":["8000"],"max_tunnels":5,)"
        R"("max_connections":7,"max_idle_workers":9})"});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code(), ErrorCode::database_error);

    // Upsert body validation failures.
    const auto upsert = [&](const std::string_view body) {
        return fixture.call("PUT", "/v1/clients/" + std::string{kSecondClientId}, body);
    };
    EXPECT_EQ(upsert("").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert("[]").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(R"({"psk":"x","psk":"y"})").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(R"({"enabled":1})").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(R"({"allowed_ports":[]})").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(R"({"allowed_ports":[1]})").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(R"({"max_tunnels":-1})").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(R"({"max_tunnels":"many"})").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(R"({"certificate_sha256":7})").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(R"({"certificate_san":7})").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(R"({"psk":""})").error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(upsert(std::string{R"({"psk":")"} + std::string(70'000U, 'x') + R"("})")
                  .error()
                  .code(),
              ErrorCode::invalid_argument);
}

TEST(PolicyManagementTest, ReloadsFromDiskAndRejectsInvalidMutations) {
    Fixture fixture;
    const std::string first = std::string{kFirstClientId};

    auto reloaded = fixture.call("POST", "/v1/reload");
    ASSERT_TRUE(reloaded) << reloaded.error();
    EXPECT_TRUE(Json::parse(reloaded->body).at("changed").empty());

    write_private(fixture.temporary.directory() / "clients.json", policy_json(false), 0640);
    reloaded = fixture.call("POST", "/v1/reload");
    ASSERT_TRUE(reloaded) << reloaded.error();
    const auto changed = Json::parse(reloaded->body).at("changed");
    ASSERT_EQ(changed.size(), 1U);
    EXPECT_EQ(changed.at(0), first);

    // Body validation failures map to the underlying error codes.
    const auto not_json = fixture.call("PUT", "/v1/clients/" + first, "not-json");
    ASSERT_FALSE(not_json);
    EXPECT_EQ(not_json.error().code(), ErrorCode::invalid_argument);
    const auto unknown_field = fixture.call("PUT", "/v1/clients/" + first, R"({"extra":1})");
    ASSERT_FALSE(unknown_field);
    const auto wrong_id = fixture.call(
        "PUT", "/v1/clients/" + first,
        R"({"client_id":"client_00000000000000000000000000000009","enabled":true})");
    ASSERT_FALSE(wrong_id);
    const auto missing_fields =
        fixture.call("PUT", "/v1/clients/" + std::string{kSecondClientId}, R"({"enabled":true})");
    ASSERT_FALSE(missing_fields);
    EXPECT_EQ(missing_fields.error().code(), ErrorCode::invalid_argument);
    const auto bad_psk = fixture.call(
        "PUT", "/v1/clients/" + std::string{kSecondClientId},
        R"({"psk":5,"allowed_ports":["8000"],"max_tunnels":5,"max_connections":7,"max_idle_workers":9})");
    ASSERT_FALSE(bad_psk);
    // A failed create must not leave a client behind.
    EXPECT_EQ(fixture.store->find(kSecondClientId), nullptr);

    const auto cert_conflict = fixture.call(
        "PUT", "/v1/clients/" + std::string{kSecondClientId},
        R"({"certificate_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
        R"("certificate_san":"DNS:x.example","allowed_ports":["8000"],"max_tunnels":5,)"
        R"("max_connections":7,"max_idle_workers":9})");
    ASSERT_FALSE(cert_conflict);
}

} // namespace
} // namespace minitun::server
