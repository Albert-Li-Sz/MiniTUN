#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/server/client_policy.hpp>

#include "storage_test_support.hpp"

namespace minitun::server {
namespace {

using storage::test::TemporaryDatabaseFile;

inline constexpr std::string_view kClientId{"client_00000000000000000000000000000001"};

[[nodiscard]] ClientPolicyLimits limits() {
    return {
        .max_clients = 10U,
        .max_tunnels_per_client = 20U,
        .max_connections_per_client = 30U,
        .max_idle_workers_per_client = 40U,
    };
}

void write_private(const std::filesystem::path& path, const std::string_view contents,
                   const mode_t mode) {
    storage::test::write_binary_file(path, contents);
    ASSERT_EQ(::chmod(path.c_str(), mode), 0);
}

[[nodiscard]] std::string policy_json(const bool enabled = true,
                                      const std::string_view extra = {}) {
    return std::string{"{"} + R"("format_version":1,"clients":[{"client_id":")" +
           std::string{kClientId} + R"(","enabled":)" + (enabled ? "true" : "false") +
           R"(,"psk_file":"client.psk","allowed_ports":["6000","7000-7099"],)" +
           R"("max_tunnels":10,"max_connections":20,"max_idle_workers":30)" + std::string{extra} +
           "}]}";
}

[[nodiscard]] std::string valid_client_json(const std::string_view client_id = kClientId,
                                            const std::string_view psk_file = "client.psk") {
    return std::string{R"({"client_id":")"} + std::string{client_id} +
           R"(","enabled":true,"psk_file":")" + std::string{psk_file} +
           R"(","allowed_ports":["6000","7000-7099"],"max_tunnels":10,"max_connections":20,"max_idle_workers":30})";
}

[[nodiscard]] std::string policy_with_clients(const std::string_view clients) {
    return std::string{R"({"format_version":1,"clients":[)"} + std::string{clients} + "]}";
}

[[nodiscard]] std::string replaced_once(std::string input, const std::string_view needle,
                                        const std::string_view replacement) {
    const auto position = input.find(needle);
    if (position == std::string::npos) {
        throw std::runtime_error("test policy replacement target is absent");
    }
    input.replace(position, needle.size(), replacement);
    return input;
}

TEST(ClientPolicyStoreTest, LoadsStrictPolicyWithRelativePskAndBoundedAclQuotas) {
    TemporaryDatabaseFile temporary;
    const auto config = temporary.directory() / "clients.json";
    write_private(temporary.directory() / "client.psk", "policy-secret\n", 0600);
    write_private(config, policy_json(true, R"(,"certificate_san":"DNS:client.example")"), 0640);

    auto store = ClientPolicyStore::open(config.string(), limits());

    ASSERT_TRUE(store) << store.error();
    EXPECT_EQ((*store)->snapshot()->size(), 1U);
    const auto policy = (*store)->find(kClientId);
    ASSERT_NE(policy, nullptr);
    EXPECT_TRUE(policy->enabled);
    ASSERT_NE(policy->psk, nullptr);
    EXPECT_EQ(policy->psk->view(), "policy-secret");
    EXPECT_TRUE(policy->allows_port(6000U));
    EXPECT_TRUE(policy->allows_port(7050U));
    EXPECT_FALSE(policy->allows_port(6999U));
    EXPECT_EQ(policy->max_tunnels, 10U);
    EXPECT_EQ(policy->max_connections, 20U);
    EXPECT_EQ(policy->max_idle_workers, 30U);
    EXPECT_EQ(policy->certificate.kind, ClientCertificateBindingKind::san);
    EXPECT_EQ(policy->certificate.value, "DNS:client.example");
    EXPECT_EQ(policy->revision_fingerprint.size(), 64U);
}

TEST(ClientPolicyStoreTest, ReloadIsAtomicAndReportsOnlyEffectiveChanges) {
    TemporaryDatabaseFile temporary;
    const auto config = temporary.directory() / "clients.json";
    write_private(temporary.directory() / "client.psk", "first-secret", 0600);
    write_private(config, policy_json(), 0640);
    auto store = ClientPolicyStore::open(config.string(), limits());
    ASSERT_TRUE(store) << store.error();
    const auto original = (*store)->find(kClientId);
    ASSERT_NE(original, nullptr);

    write_private(config, R"({"format_version":1,"unknown":[]})", 0640);
    const auto invalid = (*store)->reload();
    ASSERT_FALSE(invalid);
    EXPECT_EQ((*store)->find(kClientId)->revision_fingerprint, original->revision_fingerprint);

    write_private(temporary.directory() / "client.psk", "rotated-secret", 0600);
    write_private(config, policy_json(false), 0640);
    auto changed = (*store)->reload();
    ASSERT_TRUE(changed) << changed.error();
    ASSERT_EQ(changed->size(), 1U);
    EXPECT_EQ(changed->front(), kClientId);
    EXPECT_FALSE((*store)->find(kClientId)->enabled);
    EXPECT_NE((*store)->find(kClientId)->revision_fingerprint, original->revision_fingerprint);

    auto unchanged = (*store)->reload();
    ASSERT_TRUE(unchanged) << unchanged.error();
    EXPECT_TRUE(unchanged->empty());
}

TEST(ClientPolicyStoreTest, RejectsDuplicateFieldsOverlappingAclAndLooseSecrets) {
    TemporaryDatabaseFile temporary;
    const auto config = temporary.directory() / "clients.json";
    const auto psk = temporary.directory() / "client.psk";
    write_private(psk, "secret", 0600);

    write_private(config, R"({"format_version":1,"format_version":1,"clients":[]})", 0640);
    auto duplicate = ClientPolicyStore::open(config.string(), limits());
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::invalid_argument);

    std::string overlap = policy_json();
    const auto needle = std::string{R"("6000","7000-7099")"};
    overlap.replace(overlap.find(needle), needle.size(), R"("6000-7000","7000-7099")");
    write_private(config, overlap, 0640);
    auto overlapping = ClientPolicyStore::open(config.string(), limits());
    ASSERT_FALSE(overlapping);
    EXPECT_EQ(overlapping.error().code(), common::ErrorCode::invalid_argument);

    ASSERT_EQ(::chmod(psk.c_str(), 0644), 0);
    write_private(config, policy_json(), 0640);
    auto loose = ClientPolicyStore::open(config.string(), limits());
    ASSERT_FALSE(loose);
    EXPECT_EQ(loose.error().code(), common::ErrorCode::permission_denied);
}

TEST(ClientPolicyStoreTest, RejectsMalformedDocumentsEntriesBindingsAndQuotas) {
    TemporaryDatabaseFile temporary;
    const auto config = temporary.directory() / "clients.json";
    write_private(temporary.directory() / "client.psk", "secret", 0600);
    const std::string valid_client = valid_client_json();

    struct InvalidPolicy final {
        std::string name;
        std::string contents;
        common::ErrorCode code{common::ErrorCode::invalid_argument};
    };
    std::string too_many_ports{"["};
    for (std::size_t index = 0U; index < 257U; ++index) {
        if (index != 0U) {
            too_many_ports.push_back(',');
        }
        too_many_ports += "\"1\"";
    }
    too_many_ports.push_back(']');
    const std::string second_client_id{"client_00000000000000000000000000000002"};
    const std::vector<InvalidPolicy> cases{
        {"syntax", "not-json"},
        {"root-array", "[]"},
        {"empty-root", "{}"},
        {"unknown-root", R"({"format_version":1,"clients":[],"extra":true})"},
        {"missing-version", R"({"clients":[]})"},
        {"missing-clients", R"({"format_version":1})"},
        {"wrong-version", R"({"format_version":2,"clients":[]})"},
        {"signed-version", R"({"format_version":-1,"clients":[]})"},
        {"string-version", R"({"format_version":"1","clients":[]})"},
        {"clients-object", R"({"format_version":1,"clients":{}})"},
        {"empty-clients", R"({"format_version":1,"clients":[]})"},
        {"too-many-clients",
         policy_with_clients("null,null,null,null,null,null,null,null,null,null,null")},
        {"client-not-object", policy_with_clients("false")},
        {"client-unknown-field",
         policy_with_clients(replaced_once(valid_client, "}", ",\"extra\":true}"))},
        {"client-missing-id",
         policy_with_clients(replaced_once(
             valid_client, std::string{R"("client_id":")"} + std::string{kClientId} + "\",", ""))},
        {"client-missing-enabled",
         policy_with_clients(replaced_once(valid_client, R"("enabled":true,)", ""))},
        {"client-missing-psk",
         policy_with_clients(replaced_once(valid_client, R"("psk_file":"client.psk",)", ""))},
        {"client-missing-acl", policy_with_clients(replaced_once(
                                   valid_client, R"("allowed_ports":["6000","7000-7099"],)", ""))},
        {"client-missing-tunnels",
         policy_with_clients(replaced_once(valid_client, R"("max_tunnels":10,)", ""))},
        {"client-missing-connections",
         policy_with_clients(replaced_once(valid_client, R"("max_connections":20,)", ""))},
        {"client-missing-workers",
         policy_with_clients(replaced_once(valid_client, R"(,"max_idle_workers":30)", ""))},
        {"client-id-type",
         policy_with_clients(replaced_once(
             valid_client, std::string{R"("client_id":")"} + std::string{kClientId} + "\"",
             R"("client_id":7)"))},
        {"client-invalid-id", policy_with_clients(valid_client_json("client_invalid"))},
        {"enabled-type", policy_with_clients(replaced_once(valid_client, R"("enabled":true)",
                                                           R"("enabled":"true")"))},
        {"psk-type", policy_with_clients(replaced_once(valid_client, R"("psk_file":"client.psk")",
                                                       R"("psk_file":7)"))},
        {"psk-empty-path", policy_with_clients(valid_client_json(kClientId, ""))},
        {"psk-nul-path", policy_with_clients(valid_client_json(kClientId, "client\\u0000.psk"))},
        {"acl-type",
         policy_with_clients(replaced_once(valid_client, R"("allowed_ports":["6000","7000-7099"])",
                                           R"("allowed_ports":true)"))},
        {"acl-empty",
         policy_with_clients(replaced_once(valid_client, R"("allowed_ports":["6000","7000-7099"])",
                                           R"("allowed_ports":[])"))},
        {"acl-too-many", policy_with_clients(replaced_once(valid_client, R"(["6000","7000-7099"])",
                                                           too_many_ports))},
        {"acl-entry-type",
         policy_with_clients(replaced_once(valid_client, R"(["6000","7000-7099"])", R"([6000])"))},
        {"acl-invalid", policy_with_clients(replaced_once(valid_client, R"(["6000","7000-7099"])",
                                                          R"(["invalid"] )"))},
        {"tunnel-quota-type", policy_with_clients(replaced_once(valid_client, R"("max_tunnels":10)",
                                                                R"("max_tunnels":"10")"))},
        {"tunnel-quota-zero", policy_with_clients(replaced_once(valid_client, R"("max_tunnels":10)",
                                                                R"("max_tunnels":0)"))},
        {"tunnel-quota-large", policy_with_clients(replaced_once(
                                   valid_client, R"("max_tunnels":10)", R"("max_tunnels":21)"))},
        {"connection-quota-type",
         policy_with_clients(
             replaced_once(valid_client, R"("max_connections":20)", R"("max_connections":-1)"))},
        {"connection-quota-zero",
         policy_with_clients(
             replaced_once(valid_client, R"("max_connections":20)", R"("max_connections":0)"))},
        {"connection-quota-large",
         policy_with_clients(
             replaced_once(valid_client, R"("max_connections":20)", R"("max_connections":31)"))},
        {"worker-quota-type",
         policy_with_clients(replaced_once(valid_client, R"("max_idle_workers":30)",
                                           R"("max_idle_workers":false)"))},
        {"worker-quota-zero",
         policy_with_clients(
             replaced_once(valid_client, R"("max_idle_workers":30)", R"("max_idle_workers":0)"))},
        {"worker-quota-large",
         policy_with_clients(
             replaced_once(valid_client, R"("max_idle_workers":30)", R"("max_idle_workers":41)"))},
        {"certificate-both",
         policy_with_clients(replaced_once(valid_client, "}",
                                           ",\"certificate_sha256\":\"" + std::string(64U, 'a') +
                                               "\",\"certificate_san\":\"DNS:client.example\"}"))},
        {"fingerprint-type",
         policy_with_clients(replaced_once(valid_client, "}", ",\"certificate_sha256\":7}"))},
        {"fingerprint-short",
         policy_with_clients(replaced_once(valid_client, "}", ",\"certificate_sha256\":\"aa\"}"))},
        {"fingerprint-uppercase",
         policy_with_clients(replaced_once(
             valid_client, "}", ",\"certificate_sha256\":\"" + std::string(64U, 'A') + "\"}"))},
        {"san-type",
         policy_with_clients(replaced_once(valid_client, "}", ",\"certificate_san\":7}"))},
        {"san-prefix", policy_with_clients(replaced_once(valid_client, "}",
                                                         ",\"certificate_san\":\"OTHER:value\"}"))},
        {"san-empty",
         policy_with_clients(replaced_once(valid_client, "}", ",\"certificate_san\":\"DNS:\"}"))},
        {"san-too-long",
         policy_with_clients(replaced_once(
             valid_client, "}", ",\"certificate_san\":\"DNS:" + std::string(1'021U, 'x') + "\"}"))},
        {"san-nul", policy_with_clients(replaced_once(valid_client, "}",
                                                      ",\"certificate_san\":\"DNS:a\\u0000b\"}"))},
        {"duplicate-client", policy_with_clients(valid_client + "," + valid_client),
         common::ErrorCode::already_exists},
        {"distinct-second-client-with-missing-psk",
         policy_with_clients(valid_client + "," +
                             valid_client_json(second_client_id, "missing.psk"))},
    };

    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        write_private(config, item.contents, 0640);
        const auto opened = ClientPolicyStore::open(config.string(), limits());
        ASSERT_FALSE(opened);
        EXPECT_EQ(opened.error().code(), item.code) << opened.error();
    }
}

TEST(ClientPolicyStoreTest, EnforcesPolicyAndPskFileSafetyAndParsingLimits) {
    TemporaryDatabaseFile temporary;
    const auto directory = temporary.directory();
    const auto config = directory / "clients.json";
    const auto psk = directory / "client.psk";
    write_private(psk, "secret", 0600);
    const std::string valid_policy = policy_json();

    const auto expect_open_error = [](const std::string& path, const common::ErrorCode code) {
        const auto opened = ClientPolicyStore::open(path, limits());
        ASSERT_FALSE(opened);
        EXPECT_EQ(opened.error().code(), code) << opened.error();
    };
    expect_open_error("", common::ErrorCode::invalid_argument);
    expect_open_error(std::string(4'097U, 'x'), common::ErrorCode::invalid_argument);
    expect_open_error(std::string{"bad\0path", 8U}, common::ErrorCode::invalid_argument);
    expect_open_error((directory / "missing.json").string(), common::ErrorCode::invalid_argument);
    expect_open_error(directory.string(), common::ErrorCode::permission_denied);

    write_private(config, "", 0640);
    expect_open_error(config.string(), common::ErrorCode::invalid_argument);
    write_private(config, valid_policy, 0666);
    expect_open_error(config.string(), common::ErrorCode::permission_denied);
    write_private(config, valid_policy, 0640);

    const auto hardlink = directory / "hardlink.json";
    ASSERT_EQ(::link(config.c_str(), hardlink.c_str()), 0);
    expect_open_error(hardlink.string(), common::ErrorCode::permission_denied);
    const auto symlink = directory / "symlink.json";
    ASSERT_EQ(::symlink(config.c_str(), symlink.c_str()), 0);
    expect_open_error(symlink.string(), common::ErrorCode::invalid_argument);

    const auto bom = directory / "bom.json";
    write_private(bom, std::string{"\xef\xbb\xbf", 3U} + valid_policy, 0640);
    expect_open_error(bom.string(), common::ErrorCode::invalid_argument);
    const auto depth = directory / "depth.json";
    write_private(depth, std::string(9U, '[') + std::string(9U, ']'), 0640);
    expect_open_error(depth.string(), common::ErrorCode::resource_exhausted);
    const auto long_string = directory / "long-string.json";
    write_private(long_string, "\"" + std::string(4'097U, 'x') + "\"", 0640);
    expect_open_error(long_string.string(), common::ErrorCode::resource_exhausted);
    const auto node_limit = directory / "nodes.json";
    std::string nodes{"["};
    for (std::size_t index = 0U; index < 20'001U; ++index) {
        if (index != 0U) {
            nodes.push_back(',');
        }
        nodes.push_back('0');
    }
    nodes.push_back(']');
    write_private(node_limit, nodes, 0640);
    expect_open_error(node_limit.string(), common::ErrorCode::resource_exhausted);
    const auto oversized = directory / "oversized.json";
    write_private(oversized, std::string(1024U * 1024U + 1U, ' '), 0640);
    expect_open_error(oversized.string(), common::ErrorCode::invalid_argument);

    std::filesystem::remove(hardlink);
    write_private(config, valid_policy, 0640);
    const auto psk_hardlink = directory / "psk-hardlink";
    ASSERT_EQ(::link(psk.c_str(), psk_hardlink.c_str()), 0);
    write_private(config, policy_with_clients(valid_client_json(kClientId, "psk-hardlink")), 0640);
    expect_open_error(config.string(), common::ErrorCode::permission_denied);
    std::filesystem::remove(psk_hardlink);

    const auto psk_symlink = directory / "psk-symlink";
    ASSERT_EQ(::symlink(psk.c_str(), psk_symlink.c_str()), 0);
    write_private(config, policy_with_clients(valid_client_json(kClientId, "psk-symlink")), 0640);
    expect_open_error(config.string(), common::ErrorCode::invalid_argument);
    std::filesystem::remove(psk_symlink);

    write_private(psk, "\r\n", 0600);
    write_private(config, valid_policy, 0640);
    expect_open_error(config.string(), common::ErrorCode::invalid_argument);
    write_private(psk, std::string(64U * 1024U + 1U, 'x'), 0600);
    expect_open_error(config.string(), common::ErrorCode::invalid_argument);
}

TEST(ClientPolicyStoreTest, SupportsEveryBindingAndAtomicValidatorOutcome) {
    TemporaryDatabaseFile temporary;
    const auto config = temporary.directory() / "clients.json";
    write_private(temporary.directory() / "client.psk", "secret", 0600);
    const std::string sha(64U, 'a');
    write_private(config,
                  policy_with_clients(replaced_once(valid_client_json(), "}",
                                                    ",\"certificate_sha256\":\"" + sha + "\"}")),
                  0640);
    auto store = ClientPolicyStore::open(config.string(), limits());
    ASSERT_TRUE(store) << store.error();
    EXPECT_EQ((*store)->config_path(), config.string());
    EXPECT_TRUE((*store)->snapshot()->has_certificate_bindings());
    ASSERT_NE((*store)->find(kClientId), nullptr);
    EXPECT_EQ((*store)->find(kClientId)->certificate.kind, ClientCertificateBindingKind::sha256);
    EXPECT_EQ((*store)->find(kClientId)->certificate.value, sha);
    EXPECT_EQ((*store)->find("client_missing"), nullptr);

    const auto original = (*store)->snapshot();
    write_private(config, policy_json(false), 0640);
    const auto rejected = (*store)->reload([](const ClientPolicySnapshot&) {
        return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                             "validator rejected replacement");
    });
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), common::ErrorCode::permission_denied);
    EXPECT_EQ((*store)->snapshot(), original);

    const auto accepted = (*store)->reload([](const ClientPolicySnapshot& snapshot) {
        return snapshot.size() == 1U
                   ? common::Result<void>::success()
                   : common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                   "unexpected snapshot size");
    });
    ASSERT_TRUE(accepted) << accepted.error();
    EXPECT_FALSE((*store)->find(kClientId)->enabled);

    const ClientPolicySnapshot empty{nullptr};
    EXPECT_EQ(empty.size(), 0U);
    EXPECT_EQ(empty.find(kClientId), nullptr);
    EXPECT_FALSE(empty.has_certificate_bindings());
}

TEST(ClientPolicyStoreTest, RejectsEveryInvalidServerLimit) {
    TemporaryDatabaseFile temporary;
    const auto config = temporary.directory() / "clients.json";
    write_private(temporary.directory() / "client.psk", "secret", 0600);
    write_private(config, policy_json(), 0640);
    const std::vector<ClientPolicyLimits> invalid{
        {.max_clients = 0U,
         .max_tunnels_per_client = 1U,
         .max_connections_per_client = 1U,
         .max_idle_workers_per_client = 1U},
        {.max_clients = 100'001U,
         .max_tunnels_per_client = 1U,
         .max_connections_per_client = 1U,
         .max_idle_workers_per_client = 1U},
        {.max_clients = 1U,
         .max_tunnels_per_client = 0U,
         .max_connections_per_client = 1U,
         .max_idle_workers_per_client = 1U},
        {.max_clients = 1U,
         .max_tunnels_per_client = 4'097U,
         .max_connections_per_client = 1U,
         .max_idle_workers_per_client = 1U},
        {.max_clients = 1U,
         .max_tunnels_per_client = 1U,
         .max_connections_per_client = 0U,
         .max_idle_workers_per_client = 1U},
        {.max_clients = 1U,
         .max_tunnels_per_client = 1U,
         .max_connections_per_client = 100'001U,
         .max_idle_workers_per_client = 1U},
        {.max_clients = 1U,
         .max_tunnels_per_client = 1U,
         .max_connections_per_client = 1U,
         .max_idle_workers_per_client = 0U},
        {.max_clients = 1U,
         .max_tunnels_per_client = 1U,
         .max_connections_per_client = 1U,
         .max_idle_workers_per_client = 4'097U},
    };
    for (const auto& item : invalid) {
        const auto opened = ClientPolicyStore::open(config.string(), item);
        ASSERT_FALSE(opened);
        EXPECT_EQ(opened.error().code(), common::ErrorCode::invalid_argument);
    }
}

} // namespace
} // namespace minitun::server
