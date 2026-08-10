#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

#include "storage_test_support.hpp"

namespace minitun::storage {
namespace {

[[nodiscard]] common::Id make_id(const common::IdKind kind, const std::uint64_t number) {
    std::ostringstream suffix;
    suffix << std::hex << std::nouppercase << std::setfill('0') << std::setw(32) << number;
    auto parsed = common::Id::parse(std::string{common::id_prefix(kind)} + suffix.str(), kind);
    if (!parsed) {
        throw std::runtime_error("failed to construct deterministic test ID");
    }
    return std::move(*parsed);
}

[[nodiscard]] common::Endpoint make_endpoint(const std::string_view value) {
    auto parsed = common::Endpoint::parse(value);
    if (!parsed) {
        throw std::runtime_error("failed to construct deterministic test endpoint");
    }
    return std::move(*parsed);
}

[[nodiscard]] ServerRecord make_server(const std::uint64_t number,
                                       std::optional<std::string> name) {
    return ServerRecord{
        .id = make_id(common::IdKind::server, number),
        .name = std::move(name),
        .endpoint = make_endpoint("tunnel.example.com:2333"),
        .credential_ref = std::string{"credential-key"},
        .remote_server_id = std::string{"remote-server-id"},
        .desired_state = ServerDesiredState::enabled,
        .actual_state = ServerActualState::online,
        .last_error_code = common::ErrorCode::connection_timeout,
        .last_error_message = std::string{"previous non-sensitive timeout"},
        .reconnect_attempt = 3,
        .latency_ms = 42,
        .created_at_unix_ms = static_cast<std::int64_t>(1'000 + number),
        .updated_at_unix_ms = static_cast<std::int64_t>(2'000 + number),
    };
}

[[nodiscard]] TunnelRecord make_tunnel(const std::uint64_t number, const common::Id& server_id,
                                       const std::uint16_t remote_port,
                                       std::optional<std::string> name = std::nullopt,
                                       const std::string_view remote_host = "0.0.0.0") {
    const std::string remote = std::string{remote_host} + ':' + std::to_string(remote_port);
    return TunnelRecord{
        .id = make_id(common::IdKind::tunnel, number),
        .name = std::move(name),
        .server_id = server_id,
        .protocol = TunnelProtocol::tcp,
        .local_endpoint = make_endpoint("127.0.0.1:22"),
        .remote_endpoint = make_endpoint(remote),
        .desired_state = TunnelDesiredState::active,
        .actual_state = TunnelActualState::active,
        .last_error_code = common::ErrorCode::local_connect_failed,
        .last_error_message = std::string{"previous local failure"},
        .created_at_unix_ms = static_cast<std::int64_t>(3'000 + number),
        .updated_at_unix_ms = static_cast<std::int64_t>(4'000 + number),
        .last_synced_at_unix_ms = static_cast<std::int64_t>(3'500 + number),
    };
}

TEST(ServerRepositoryTest, RoundTripsEveryFieldAndPersistsAcrossReopen) {
    test::TemporaryDatabaseFile temporary;
    const ServerRecord first = make_server(1, "primary'; DROP TABLE servers; --");
    ServerRecord unnamed = make_server(2, std::nullopt);
    unnamed.credential_ref = std::nullopt;
    unnamed.remote_server_id = std::nullopt;
    unnamed.last_error_code = std::nullopt;
    unnamed.last_error_message = std::nullopt;
    unnamed.latency_ms = std::nullopt;
    unnamed.created_at_unix_ms = 500;
    unnamed.updated_at_unix_ms = 600;

    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();

        ASSERT_TRUE((*repository)->servers().create(first));
        ASSERT_TRUE((*repository)->servers().create(unnamed));

        const auto by_id = (*repository)->servers().get_by_id(first.id);
        ASSERT_TRUE(by_id) << by_id.error();
        EXPECT_EQ(*by_id, first);

        const auto by_name = (*repository)->servers().get_by_name(*first.name);
        ASSERT_TRUE(by_name) << by_name.error();
        EXPECT_EQ(*by_name, first);

        const auto listed = (*repository)->servers().list();
        ASSERT_TRUE(listed) << listed.error();
        ASSERT_EQ(listed->size(), 2U);
        EXPECT_EQ((*listed)[0], unnamed);
        EXPECT_EQ((*listed)[1], first);
    }

    auto reopened = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(reopened) << reopened.error();
    const auto restored = (*reopened)->servers().get_by_id(first.id);
    ASSERT_TRUE(restored) << restored.error();
    EXPECT_EQ(*restored, first);
}

TEST(ServerRepositoryTest, UpdatesAtomicallyAndEnforcesUniqueKeys) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    ServerRecord first = make_server(10, "primary");
    ServerRecord second = make_server(11, "backup");
    ASSERT_TRUE((*repository)->servers().create(first));
    ASSERT_TRUE((*repository)->servers().create(second));

    const auto duplicate_id = (*repository)->servers().create(first);
    ASSERT_FALSE(duplicate_id);
    EXPECT_EQ(duplicate_id.error().code(), common::ErrorCode::already_exists);

    ServerRecord duplicate_name = make_server(12, "primary");
    const auto duplicate_name_result = (*repository)->servers().create(duplicate_name);
    ASSERT_FALSE(duplicate_name_result);
    EXPECT_EQ(duplicate_name_result.error().code(), common::ErrorCode::already_exists);

    second.name = first.name;
    second.endpoint = make_endpoint("[2001:db8::10]:2444");
    second.updated_at_unix_ms += 100;
    const auto conflicting_update = (*repository)->servers().update(second);
    ASSERT_FALSE(conflicting_update);
    EXPECT_EQ(conflicting_update.error().code(), common::ErrorCode::already_exists);

    const auto unchanged = (*repository)->servers().get_by_id(second.id);
    ASSERT_TRUE(unchanged) << unchanged.error();
    EXPECT_EQ(unchanged->name, std::optional<std::string>{"backup"});
    EXPECT_EQ(unchanged->endpoint.to_string(), "tunnel.example.com:2333");

    first.endpoint = make_endpoint("[2001:db8::1]:443");
    first.credential_ref = "new-reference";
    first.remote_server_id = "new-remote-id";
    first.actual_state = ServerActualState::backoff;
    first.last_error_code = common::ErrorCode::connection_failed;
    first.last_error_message = "new failure";
    first.reconnect_attempt = 8;
    first.latency_ms = 99;
    first.updated_at_unix_ms += 500;
    ASSERT_TRUE((*repository)->servers().update(first));

    const auto updated = (*repository)->servers().get_by_id(first.id);
    ASSERT_TRUE(updated) << updated.error();
    EXPECT_EQ(*updated, first);
}

TEST(ServerRepositoryTest, EnforcesInputAndConfiguredCountLimits) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string(),
                                            StorageLimits{.max_servers = 2, .max_tunnels = 4});
    ASSERT_TRUE(repository) << repository.error();

    ServerRecord maximum_name = make_server(20, std::string(kMaxNameBytes, 'n'));
    ASSERT_TRUE((*repository)->servers().create(maximum_name));
    ASSERT_TRUE((*repository)->servers().create(make_server(21, std::nullopt)));

    const auto at_capacity = (*repository)->servers().create(make_server(22, "overflow"));
    ASSERT_FALSE(at_capacity);
    EXPECT_EQ(at_capacity.error().code(), common::ErrorCode::resource_exhausted);

    ServerRecord oversized = make_server(23, std::string(kMaxNameBytes + 1U, 'x'));
    const auto oversized_result = (*repository)->servers().update(oversized);
    ASSERT_FALSE(oversized_result);
    EXPECT_EQ(oversized_result.error().code(), common::ErrorCode::invalid_argument);

    std::string invalid_utf8{"bad"};
    invalid_utf8.push_back(static_cast<char>(0xffU));
    ServerRecord malformed = make_server(24, invalid_utf8);
    const auto malformed_result = (*repository)->servers().update(malformed);
    ASSERT_FALSE(malformed_result);
    EXPECT_EQ(malformed_result.error().code(), common::ErrorCode::invalid_argument);

    ServerRecord wrong_kind = make_server(25, "wrong-kind");
    wrong_kind.id = make_id(common::IdKind::tunnel, 25);
    const auto wrong_kind_result = (*repository)->servers().update(wrong_kind);
    ASSERT_FALSE(wrong_kind_result);
    EXPECT_EQ(wrong_kind_result.error().code(), common::ErrorCode::invalid_argument);
}

TEST(ServerRepositoryTest, RejectsEveryInvalidRecordFieldBeforePersistence) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const auto expect_invalid = [&repository](const ServerRecord& record) {
        const auto created = (*repository)->servers().create(record);
        ASSERT_FALSE(created);
        EXPECT_EQ(created.error().code(), common::ErrorCode::invalid_argument) << created.error();
    };
    std::vector<ServerRecord> invalid;
    auto add = [&invalid](ServerRecord value) { invalid.push_back(std::move(value)); };

    ServerRecord value = make_server(100, "valid");
    value.id = make_id(common::IdKind::tunnel, 100);
    add(value);
    value = make_server(101, "");
    add(value);
    value = make_server(102, std::string(kMaxNameBytes + 1U, 'x'));
    add(value);
    value = make_server(103, std::string{"a\0b", 3U});
    add(value);
    value = make_server(104, std::string{"\xff", 1U});
    add(value);
    value = make_server(105, "valid");
    value.credential_ref = "";
    add(value);
    value = make_server(106, "valid");
    value.remote_server_id = std::string(kMaxRemoteServerIdBytes + 1U, 'x');
    add(value);
    value = make_server(107, "valid");
    value.tls_server_name = std::string{"bad\0name", 8U};
    add(value);
    value = make_server(108, "valid");
    value.ca_credential_ref = std::string(kMaxCredentialReferenceBytes + 1U, 'x');
    add(value);
    value = make_server(109, "valid");
    value.client_certificate_ref = "certificate";
    add(value);
    value = make_server(110, "valid");
    value.config_revision = 0U;
    add(value);
    value = make_server(111, "valid");
    value.config_revision =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
    add(value);
    value = make_server(112, "valid");
    value.desired_state = static_cast<ServerDesiredState>(255U);
    add(value);
    value = make_server(113, "valid");
    value.actual_state = static_cast<ServerActualState>(255U);
    add(value);
    value = make_server(114, "valid");
    value.last_error_code = common::ErrorCode::ok;
    add(value);
    value = make_server(115, "valid");
    value.last_error_code = static_cast<common::ErrorCode>(255U);
    add(value);
    value = make_server(116, "valid");
    value.last_error_message = std::string(kMaxErrorMessageBytes + 1U, 'x');
    add(value);
    value = make_server(117, "valid");
    value.reconnect_attempt =
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) + 1U;
    add(value);
    value = make_server(118, "valid");
    value.latency_ms = -1;
    add(value);
    value = make_server(119, "valid");
    value.latency_ms = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1;
    add(value);
    value = make_server(120, "valid");
    value.created_at_unix_ms = -1;
    add(value);
    value = make_server(121, "valid");
    value.updated_at_unix_ms = value.created_at_unix_ms - 1;
    add(value);

    for (const auto& record : invalid) {
        SCOPED_TRACE(record.id.str());
        expect_invalid(record);
    }
    auto listed = (*repository)->servers().list();
    ASSERT_TRUE(listed) << listed.error();
    EXPECT_TRUE(listed->empty());
}

TEST(TunnelRepositoryTest, EnforcesPerServerRemoteBindingAndForeignKeys) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord first_server = make_server(30, "first");
    const ServerRecord second_server = make_server(31, "second");
    ASSERT_TRUE((*repository)->servers().create(first_server));
    ASSERT_TRUE((*repository)->servers().create(second_server));

    const TunnelRecord first = make_tunnel(30, first_server.id, 6'000, "ssh");
    const TunnelRecord same_binding_other_server = make_tunnel(31, second_server.id, 6'000, "ssh");
    const TunnelRecord same_port_other_host =
        make_tunnel(32, first_server.id, 6'000, "other-host", "127.0.0.1");
    ASSERT_TRUE((*repository)->tunnels().create(first));
    ASSERT_TRUE((*repository)->tunnels().create(same_binding_other_server));
    ASSERT_TRUE((*repository)->tunnels().create(same_port_other_host));

    TunnelRecord duplicate_binding = make_tunnel(33, first_server.id, 6'000, "duplicate");
    const auto duplicate = (*repository)->tunnels().create(duplicate_binding);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::already_exists);

    TunnelRecord missing_parent =
        make_tunnel(34, make_id(common::IdKind::server, 999), 6'001, "orphan");
    const auto orphan = (*repository)->tunnels().create(missing_parent);
    ASSERT_FALSE(orphan);
    EXPECT_EQ(orphan.error().code(), common::ErrorCode::not_found);

    const auto first_server_tunnels = (*repository)->tunnels().list_by_server(first_server.id);
    ASSERT_TRUE(first_server_tunnels) << first_server_tunnels.error();
    EXPECT_EQ(first_server_tunnels->size(), 2U);

    const auto ambiguous = (*repository)->tunnels().get_by_name("ssh");
    ASSERT_FALSE(ambiguous);
    EXPECT_EQ(ambiguous.error().code(), common::ErrorCode::invalid_argument);
}

TEST(TunnelRepositoryTest, RejectsEveryInvalidRecordAndTransitionArgument) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();
    const ServerRecord server = make_server(130, "validation");
    ASSERT_TRUE((*repository)->servers().create(server));

    const auto expect_invalid = [&repository](const TunnelRecord& record) {
        const auto created = (*repository)->tunnels().create(record);
        ASSERT_FALSE(created);
        EXPECT_EQ(created.error().code(), common::ErrorCode::invalid_argument) << created.error();
    };
    std::vector<TunnelRecord> invalid;
    auto add = [&invalid](TunnelRecord value) { invalid.push_back(std::move(value)); };
    TunnelRecord value = make_tunnel(130, server.id, 7'000U, "valid");
    value.id = make_id(common::IdKind::server, 130);
    add(value);
    value = make_tunnel(131, make_id(common::IdKind::tunnel, 1), 7'001U, "valid");
    add(value);
    value = make_tunnel(132, server.id, 7'002U, "");
    add(value);
    value = make_tunnel(133, server.id, 7'003U, std::string(kMaxNameBytes + 1U, 'x'));
    add(value);
    value = make_tunnel(134, server.id, 7'004U, std::string{"\xff", 1U});
    add(value);
    value = make_tunnel(135, server.id, 7'005U, "valid");
    value.protocol = static_cast<TunnelProtocol>(255U);
    add(value);
    value = make_tunnel(136, server.id, 7'006U, "valid");
    value.desired_state = static_cast<TunnelDesiredState>(255U);
    add(value);
    value = make_tunnel(137, server.id, 7'007U, "valid");
    value.actual_state = static_cast<TunnelActualState>(255U);
    add(value);
    value = make_tunnel(138, server.id, 7'008U, "valid");
    value.last_error_code = common::ErrorCode::ok;
    add(value);
    value = make_tunnel(139, server.id, 7'009U, "valid");
    value.last_error_message = std::string(kMaxErrorMessageBytes + 1U, 'x');
    add(value);
    value = make_tunnel(140, server.id, 7'010U, "valid");
    value.config_revision = 0U;
    add(value);
    value = make_tunnel(141, server.id, 7'011U, "valid");
    value.config_revision =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
    add(value);
    value = make_tunnel(142, server.id, 7'012U, "valid");
    value.created_at_unix_ms = -1;
    add(value);
    value = make_tunnel(143, server.id, 7'013U, "valid");
    value.updated_at_unix_ms = value.created_at_unix_ms - 1;
    add(value);
    value = make_tunnel(144, server.id, 7'014U, "valid");
    value.last_synced_at_unix_ms = value.created_at_unix_ms - 1;
    add(value);
    value = make_tunnel(145, server.id, 7'015U, "valid");
    value.last_synced_at_unix_ms = value.updated_at_unix_ms + 1;
    add(value);

    for (const auto& record : invalid) {
        SCOPED_TRACE(record.id.str());
        expect_invalid(record);
    }

    const auto wrong_server_id = make_id(common::IdKind::tunnel, 999U);
    const auto wrong_tunnel_id = make_id(common::IdKind::server, 999U);
    EXPECT_EQ((*repository)->tunnels().list_by_server(wrong_server_id).error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*repository)->tunnels().get_by_id(wrong_tunnel_id).error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*repository)->tunnels().get_by_name("").error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ(
        (*repository)->tunnels().get_by_name(std::string(kMaxNameBytes + 1U, 'x')).error().code(),
        common::ErrorCode::invalid_argument);
    EXPECT_EQ((*repository)->tunnels().mark_removed(wrong_tunnel_id, 1).error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*repository)->tunnels().erase(wrong_tunnel_id).error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*repository)
                  ->tunnels()
                  .mark_active_pending_by_server(wrong_server_id, std::nullopt, 1)
                  .error()
                  .code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*repository)
                  ->tunnels()
                  .mark_active_pending_by_server(server.id, std::nullopt, -1)
                  .error()
                  .code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*repository)
                  ->tunnels()
                  .mark_active_pending_by_server(server.id,
                                                 common::Error{common::ErrorCode::ok, "invalid"}, 1)
                  .error()
                  .code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*repository)
                  ->tunnels()
                  .mark_active_pending_by_server(
                      server.id,
                      common::Error{common::ErrorCode::connection_failed,
                                    std::string(kMaxErrorMessageBytes + 1U, 'x')},
                      1)
                  .error()
                  .code(),
              common::ErrorCode::invalid_argument);

    const TunnelRecord tunnel = make_tunnel(150, server.id, 7'100U, "conditional");
    ASSERT_TRUE((*repository)->tunnels().create(tunnel));
    const auto conditional = [&repository, &tunnel,
                              &server](const common::Id& id, const common::Id& server_id,
                                       const std::uint64_t revision, const TunnelActualState state,
                                       const std::optional<common::Error>& error,
                                       const std::int64_t updated_at) {
        return (*repository)
            ->tunnels()
            .update_runtime_state_if_revision(id, server_id, revision, state, error, updated_at,
                                              false);
    };
    EXPECT_FALSE(
        conditional(wrong_tunnel_id, server.id, 1U, TunnelActualState::pending, std::nullopt, 1));
    EXPECT_FALSE(
        conditional(tunnel.id, wrong_server_id, 1U, TunnelActualState::pending, std::nullopt, 1));
    EXPECT_FALSE(
        conditional(tunnel.id, server.id, 0U, TunnelActualState::pending, std::nullopt, 1));
    EXPECT_FALSE(
        conditional(tunnel.id, server.id,
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U,
                    TunnelActualState::pending, std::nullopt, 1));
    EXPECT_FALSE(
        conditional(tunnel.id, server.id, 1U, TunnelActualState::removing, std::nullopt, 1));
    EXPECT_FALSE(
        conditional(tunnel.id, server.id, 1U, TunnelActualState::pending, std::nullopt, -1));
    EXPECT_FALSE(conditional(tunnel.id, server.id, 1U, TunnelActualState::pending,
                             common::Error{common::ErrorCode::ok, "invalid"}, 1));
    EXPECT_FALSE(conditional(tunnel.id, server.id, 1U, TunnelActualState::pending,
                             common::Error{common::ErrorCode::connection_failed,
                                           std::string(kMaxErrorMessageBytes + 1U, 'x')},
                             1));
}

TEST(StorageRepositoryCorruptionTest, RejectsMalformedServerAndTunnelRowsFieldByField) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();
    const ServerRecord server = make_server(160, "corrupt-server");
    const TunnelRecord tunnel = make_tunnel(160, server.id, 7'200U, "corrupt-tunnel");
    ASSERT_TRUE((*repository)->servers().create(server));
    ASSERT_TRUE((*repository)->tunnels().create(tunnel));

    test::NativeSqliteDatabase injector{temporary.path()};
    injector.execute("PRAGMA ignore_check_constraints = ON");
    struct Corruption final {
        std::string name;
        std::string corrupt;
        std::string restore;
    };
    const std::string server_where = " WHERE id = '" + server.id.str() + "'";
    const std::vector<Corruption> server_corruptions{
        {"id", "UPDATE servers SET id = 'server_invalid'" + server_where,
         "UPDATE servers SET id = '" + server.id.str() + "' WHERE id = 'server_invalid'"},
        {"name-type", "UPDATE servers SET name = X'37'" + server_where,
         "UPDATE servers SET name = 'corrupt-server'" + server_where},
        {"endpoint-type", "UPDATE servers SET endpoint = X'37'" + server_where,
         "UPDATE servers SET endpoint = 'tunnel.example.com:2333'" + server_where},
        {"endpoint-canonical",
         "UPDATE servers SET endpoint = 'TUNNEL.EXAMPLE.COM:2333'" + server_where,
         "UPDATE servers SET endpoint = 'tunnel.example.com:2333'" + server_where},
        {"credential-type", "UPDATE servers SET credential_ref = X'37'" + server_where,
         "UPDATE servers SET credential_ref = 'credential-key'" + server_where},
        {"remote-id-type", "UPDATE servers SET remote_server_id = X'37'" + server_where,
         "UPDATE servers SET remote_server_id = 'remote-server-id'" + server_where},
        {"desired-state", "UPDATE servers SET desired_state = 'invalid'" + server_where,
         "UPDATE servers SET desired_state = 'enabled'" + server_where},
        {"actual-state", "UPDATE servers SET actual_state = 'invalid'" + server_where,
         "UPDATE servers SET actual_state = 'online'" + server_where},
        {"error-code", "UPDATE servers SET last_error_code = 'ok'" + server_where,
         "UPDATE servers SET last_error_code = 'connection_timeout'" + server_where},
        {"error-message-type", "UPDATE servers SET last_error_message = X'37'" + server_where,
         "UPDATE servers SET last_error_message = 'previous non-sensitive timeout'" + server_where},
        {"reconnect-type", "UPDATE servers SET reconnect_attempt = 'three'" + server_where,
         "UPDATE servers SET reconnect_attempt = 3" + server_where},
        {"reconnect-negative", "UPDATE servers SET reconnect_attempt = -1" + server_where,
         "UPDATE servers SET reconnect_attempt = 3" + server_where},
        {"reconnect-large", "UPDATE servers SET reconnect_attempt = 2147483648" + server_where,
         "UPDATE servers SET reconnect_attempt = 3" + server_where},
        {"latency-type", "UPDATE servers SET latency_ms = 'slow'" + server_where,
         "UPDATE servers SET latency_ms = 42" + server_where},
        {"latency-negative", "UPDATE servers SET latency_ms = -1" + server_where,
         "UPDATE servers SET latency_ms = 42" + server_where},
        {"created-type", "UPDATE servers SET created_at = 'now'" + server_where,
         "UPDATE servers SET created_at = 1160" + server_where},
        {"created-negative", "UPDATE servers SET created_at = -1" + server_where,
         "UPDATE servers SET created_at = 1160" + server_where},
        {"updated-before-created", "UPDATE servers SET updated_at = 1" + server_where,
         "UPDATE servers SET updated_at = 2160" + server_where},
        {"tls-type", "UPDATE servers SET tls_server_name = X'37'" + server_where,
         "UPDATE servers SET tls_server_name = NULL" + server_where},
        {"ca-type", "UPDATE servers SET ca_credential_ref = X'37'" + server_where,
         "UPDATE servers SET ca_credential_ref = NULL" + server_where},
        {"certificate-type", "UPDATE servers SET client_certificate_ref = X'37'" + server_where,
         "UPDATE servers SET client_certificate_ref = NULL" + server_where},
        {"private-key-type", "UPDATE servers SET client_private_key_ref = X'37'" + server_where,
         "UPDATE servers SET client_private_key_ref = NULL" + server_where},
        {"revision-type", "UPDATE servers SET config_revision = 'one'" + server_where,
         "UPDATE servers SET config_revision = 1" + server_where},
        {"revision-zero", "UPDATE servers SET config_revision = 0" + server_where,
         "UPDATE servers SET config_revision = 1" + server_where},
        {"ownership", "UPDATE servers SET managed_by_config = 2" + server_where,
         "UPDATE servers SET managed_by_config = 0" + server_where},
    };
    for (const auto& item : server_corruptions) {
        SCOPED_TRACE("server " + item.name);
        injector.execute(item.corrupt);
        const auto listed = (*repository)->servers().list();
        ASSERT_FALSE(listed);
        EXPECT_EQ(listed.error().code(), common::ErrorCode::database_error) << listed.error();
        injector.execute(item.restore);
        ASSERT_TRUE((*repository)->servers().get_by_id(server.id));
    }

    const std::string tunnel_where = " WHERE id = '" + tunnel.id.str() + "'";
    const std::vector<Corruption> tunnel_corruptions{
        {"id", "UPDATE tunnels SET id = 'tunnel_invalid'" + tunnel_where,
         "UPDATE tunnels SET id = '" + tunnel.id.str() + "' WHERE id = 'tunnel_invalid'"},
        {"name-type", "UPDATE tunnels SET name = X'37'" + tunnel_where,
         "UPDATE tunnels SET name = 'corrupt-tunnel'" + tunnel_where},
        {"server-id-type", "UPDATE tunnels SET server_id = 7" + tunnel_where,
         "UPDATE tunnels SET server_id = '" + server.id.str() + "'" + tunnel_where},
        {"server-id-value", "UPDATE tunnels SET server_id = 'server_invalid'" + tunnel_where,
         "UPDATE tunnels SET server_id = '" + server.id.str() + "'" + tunnel_where},
        {"protocol-type", "UPDATE tunnels SET protocol = 7" + tunnel_where,
         "UPDATE tunnels SET protocol = 'tcp'" + tunnel_where},
        {"protocol-value", "UPDATE tunnels SET protocol = 'udp'" + tunnel_where,
         "UPDATE tunnels SET protocol = 'tcp'" + tunnel_where},
        {"local-host-type", "UPDATE tunnels SET local_host = X'37'" + tunnel_where,
         "UPDATE tunnels SET local_host = '127.0.0.1'" + tunnel_where},
        {"local-host-value", "UPDATE tunnels SET local_host = 'LOCALHOST'" + tunnel_where,
         "UPDATE tunnels SET local_host = '127.0.0.1'" + tunnel_where},
        {"local-port-type", "UPDATE tunnels SET local_port = 'twenty-two'" + tunnel_where,
         "UPDATE tunnels SET local_port = 22" + tunnel_where},
        {"local-port-zero", "UPDATE tunnels SET local_port = 0" + tunnel_where,
         "UPDATE tunnels SET local_port = 22" + tunnel_where},
        {"remote-host-type", "UPDATE tunnels SET remote_host = X'37'" + tunnel_where,
         "UPDATE tunnels SET remote_host = '0.0.0.0'" + tunnel_where},
        {"remote-port-type", "UPDATE tunnels SET remote_port = 'port'" + tunnel_where,
         "UPDATE tunnels SET remote_port = 7200" + tunnel_where},
        {"remote-port-large", "UPDATE tunnels SET remote_port = 65536" + tunnel_where,
         "UPDATE tunnels SET remote_port = 7200" + tunnel_where},
        {"desired-state", "UPDATE tunnels SET desired_state = 'invalid'" + tunnel_where,
         "UPDATE tunnels SET desired_state = 'active'" + tunnel_where},
        {"actual-state", "UPDATE tunnels SET actual_state = 'invalid'" + tunnel_where,
         "UPDATE tunnels SET actual_state = 'active'" + tunnel_where},
        {"error-code", "UPDATE tunnels SET last_error_code = 'ok'" + tunnel_where,
         "UPDATE tunnels SET last_error_code = 'local_connect_failed'" + tunnel_where},
        {"error-message-type", "UPDATE tunnels SET last_error_message = X'37'" + tunnel_where,
         "UPDATE tunnels SET last_error_message = 'previous local failure'" + tunnel_where},
        {"created-type", "UPDATE tunnels SET created_at = 'now'" + tunnel_where,
         "UPDATE tunnels SET created_at = 3160" + tunnel_where},
        {"created-negative", "UPDATE tunnels SET created_at = -1" + tunnel_where,
         "UPDATE tunnels SET created_at = 3160" + tunnel_where},
        {"updated-before-created", "UPDATE tunnels SET updated_at = 1" + tunnel_where,
         "UPDATE tunnels SET updated_at = 4160" + tunnel_where},
        {"last-synced-type", "UPDATE tunnels SET last_synced_at = 'now'" + tunnel_where,
         "UPDATE tunnels SET last_synced_at = 3660" + tunnel_where},
        {"last-synced-early", "UPDATE tunnels SET last_synced_at = 1" + tunnel_where,
         "UPDATE tunnels SET last_synced_at = 3660" + tunnel_where},
        {"last-synced-late", "UPDATE tunnels SET last_synced_at = 9999" + tunnel_where,
         "UPDATE tunnels SET last_synced_at = 3660" + tunnel_where},
        {"revision-type", "UPDATE tunnels SET config_revision = 'one'" + tunnel_where,
         "UPDATE tunnels SET config_revision = 1" + tunnel_where},
        {"revision-zero", "UPDATE tunnels SET config_revision = 0" + tunnel_where,
         "UPDATE tunnels SET config_revision = 1" + tunnel_where},
        {"ownership", "UPDATE tunnels SET managed_by_config = 2" + tunnel_where,
         "UPDATE tunnels SET managed_by_config = 0" + tunnel_where},
    };
    for (const auto& item : tunnel_corruptions) {
        SCOPED_TRACE("tunnel " + item.name);
        injector.execute(item.corrupt);
        const auto listed = (*repository)->tunnels().list();
        ASSERT_FALSE(listed);
        EXPECT_EQ(listed.error().code(), common::ErrorCode::database_error) << listed.error();
        injector.execute(item.restore);
        ASSERT_TRUE((*repository)->tunnels().get_by_id(tunnel.id));
    }
}

TEST(TunnelRepositoryTest, RoundTripsUpdatesTombstonesAndCascadingErase) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord server = make_server(40, "primary");
    ASSERT_TRUE((*repository)->servers().create(server));
    TunnelRecord first = make_tunnel(40, server.id, 6'100, "first");
    const TunnelRecord second = make_tunnel(41, server.id, 6'101, "second");
    ASSERT_TRUE((*repository)->tunnels().create(first));
    ASSERT_TRUE((*repository)->tunnels().create(second));

    const auto loaded = (*repository)->tunnels().get_by_id(first.id);
    ASSERT_TRUE(loaded) << loaded.error();
    EXPECT_EQ(*loaded, first);

    first.local_endpoint = make_endpoint("[::1]:8080");
    first.remote_endpoint = make_endpoint("127.0.0.1:6200");
    first.actual_state = TunnelActualState::failed;
    first.last_error_message = "updated failure";
    first.updated_at_unix_ms += 100;
    ASSERT_TRUE((*repository)->tunnels().update(first));
    const auto updated = (*repository)->tunnels().get_by_id(first.id);
    ASSERT_TRUE(updated) << updated.error();
    EXPECT_EQ(*updated, first);

    ASSERT_TRUE((*repository)->tunnels().mark_removed(first.id, first.updated_at_unix_ms + 1));
    const auto tombstone = (*repository)->tunnels().get_by_id(first.id);
    ASSERT_TRUE(tombstone) << tombstone.error();
    EXPECT_EQ(tombstone->desired_state, TunnelDesiredState::removed);
    EXPECT_EQ(tombstone->actual_state, TunnelActualState::removing);

    ASSERT_TRUE((*repository)->servers().mark_removed(server.id, second.updated_at_unix_ms + 1));
    const auto child_tombstone = (*repository)->tunnels().get_by_id(second.id);
    ASSERT_TRUE(child_tombstone) << child_tombstone.error();
    EXPECT_EQ(child_tombstone->desired_state, TunnelDesiredState::removed);
    EXPECT_EQ(child_tombstone->actual_state, TunnelActualState::removing);

    ASSERT_TRUE((*repository)->servers().erase(server.id));
    const auto cascaded = (*repository)->tunnels().get_by_id(second.id);
    ASSERT_FALSE(cascaded);
    EXPECT_EQ(cascaded.error().code(), common::ErrorCode::not_found);
}

TEST(TunnelRepositoryTest, BatchMarksOnlyActiveServerTunnelsPending) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord server = make_server(43, "batch");
    const ServerRecord other_server = make_server(44, "other");
    ASSERT_TRUE((*repository)->servers().create(server));
    ASSERT_TRUE((*repository)->servers().create(other_server));

    const TunnelRecord first = make_tunnel(43, server.id, 6'130, "first");
    const TunnelRecord second = make_tunnel(44, server.id, 6'131, "second");
    TunnelRecord disabled = make_tunnel(45, server.id, 6'132, "disabled");
    disabled.desired_state = TunnelDesiredState::disabled;
    disabled.actual_state = TunnelActualState::disabled;
    const TunnelRecord unrelated = make_tunnel(46, other_server.id, 6'133, "unrelated");
    ASSERT_TRUE((*repository)->tunnels().create(first));
    ASSERT_TRUE((*repository)->tunnels().create(second));
    ASSERT_TRUE((*repository)->tunnels().create(disabled));
    ASSERT_TRUE((*repository)->tunnels().create(unrelated));

    const common::Error disconnect{common::ErrorCode::connection_failed,
                                   "remote control session closed"};
    auto updated =
        (*repository)->tunnels().mark_active_pending_by_server(server.id, disconnect, 9'000);
    ASSERT_TRUE(updated) << updated.error();
    EXPECT_EQ(*updated, 2U);

    for (const auto& id : {first.id, second.id}) {
        const auto tunnel = (*repository)->tunnels().get_by_id(id);
        ASSERT_TRUE(tunnel) << tunnel.error();
        EXPECT_EQ(tunnel->actual_state, TunnelActualState::pending);
        EXPECT_EQ(tunnel->last_error_code, common::ErrorCode::connection_failed);
        EXPECT_EQ(tunnel->last_error_message, "remote control session closed");
        EXPECT_EQ(tunnel->updated_at_unix_ms, 9'000);
        EXPECT_EQ(tunnel->last_synced_at_unix_ms,
                  id == first.id ? first.last_synced_at_unix_ms : second.last_synced_at_unix_ms);
    }
    EXPECT_EQ(*(*repository)->tunnels().get_by_id(disabled.id), disabled);
    EXPECT_EQ(*(*repository)->tunnels().get_by_id(unrelated.id), unrelated);
}

TEST(TunnelRepositoryTest, BatchPendingFailureIsReturnedWithoutPartialUpdates) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord server = make_server(47, "batch-failure");
    const TunnelRecord first = make_tunnel(47, server.id, 6'140, "first");
    const TunnelRecord second = make_tunnel(48, server.id, 6'141, "second");
    ASSERT_TRUE((*repository)->servers().create(server));
    ASSERT_TRUE((*repository)->tunnels().create(first));
    ASSERT_TRUE((*repository)->tunnels().create(second));

    test::NativeSqliteDatabase injector{temporary.path()};
    injector.execute("CREATE TRIGGER reject_batch_pending BEFORE UPDATE OF actual_state ON tunnels "
                     "WHEN NEW.actual_state = 'pending' BEGIN "
                     "SELECT RAISE(ABORT, 'injected batch failure'); END");
    const auto updated =
        (*repository)
            ->tunnels()
            .mark_active_pending_by_server(server.id,
                                           common::Error{common::ErrorCode::connection_failed,
                                                         "remote control session closed"},
                                           9'000);
    ASSERT_FALSE(updated);
    EXPECT_EQ(updated.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(*(*repository)->tunnels().get_by_id(first.id), first);
    EXPECT_EQ(*(*repository)->tunnels().get_by_id(second.id), second);
}

TEST(StorageRepositoryInvariantTest, RejectsTimeRegressionAndPrematurePhysicalErase) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    ServerRecord server = make_server(45, "guarded");
    TunnelRecord tunnel = make_tunnel(45, server.id, 6'150, "guarded");
    ASSERT_TRUE((*repository)->servers().create(server));
    ASSERT_TRUE((*repository)->tunnels().create(tunnel));

    const auto active_tunnel_erase = (*repository)->tunnels().erase(tunnel.id);
    ASSERT_FALSE(active_tunnel_erase);
    EXPECT_EQ(active_tunnel_erase.error().code(), common::ErrorCode::invalid_argument);

    const auto active_server_erase = (*repository)->servers().erase(server.id);
    ASSERT_FALSE(active_server_erase);
    EXPECT_EQ(active_server_erase.error().code(), common::ErrorCode::invalid_argument);

    ServerRecord stale_server = server;
    stale_server.endpoint = make_endpoint("new.example.com:2444");
    --stale_server.updated_at_unix_ms;
    const auto stale_server_update = (*repository)->servers().update(stale_server);
    ASSERT_FALSE(stale_server_update);
    EXPECT_EQ(stale_server_update.error().code(), common::ErrorCode::invalid_argument);

    TunnelRecord stale_tunnel = tunnel;
    stale_tunnel.local_endpoint = make_endpoint("127.0.0.1:8080");
    --stale_tunnel.updated_at_unix_ms;
    const auto stale_tunnel_update = (*repository)->tunnels().update(stale_tunnel);
    ASSERT_FALSE(stale_tunnel_update);
    EXPECT_EQ(stale_tunnel_update.error().code(), common::ErrorCode::invalid_argument);

    ServerRecord rewritten_creation_time = server;
    ++rewritten_creation_time.created_at_unix_ms;
    const auto rewritten_server = (*repository)->servers().update(rewritten_creation_time);
    ASSERT_FALSE(rewritten_server);
    EXPECT_EQ(rewritten_server.error().code(), common::ErrorCode::invalid_argument);

    const auto stale_server_removal =
        (*repository)->servers().mark_removed(server.id, server.updated_at_unix_ms - 1);
    ASSERT_FALSE(stale_server_removal);
    EXPECT_EQ(stale_server_removal.error().code(), common::ErrorCode::invalid_argument);

    const auto stale_tunnel_removal =
        (*repository)->tunnels().mark_removed(tunnel.id, tunnel.updated_at_unix_ms - 1);
    ASSERT_FALSE(stale_tunnel_removal);
    EXPECT_EQ(stale_tunnel_removal.error().code(), common::ErrorCode::invalid_argument);

    server.desired_state = ServerDesiredState::removed;
    server.actual_state = ServerActualState::disabled;
    ++server.updated_at_unix_ms;
    ASSERT_TRUE((*repository)->servers().update(server));

    const auto live_child_cascade = (*repository)->servers().erase(server.id);
    ASSERT_FALSE(live_child_cascade);
    EXPECT_EQ(live_child_cascade.error().code(), common::ErrorCode::invalid_argument);

    const auto preserved_server = (*repository)->servers().get_by_id(server.id);
    ASSERT_TRUE(preserved_server) << preserved_server.error();
    const auto preserved_tunnel = (*repository)->tunnels().get_by_id(tunnel.id);
    ASSERT_TRUE(preserved_tunnel) << preserved_tunnel.error();
    EXPECT_EQ(preserved_tunnel->desired_state, TunnelDesiredState::active);
}

TEST(StorageTransactionTest, CommitsMultipleRepositoriesAtomically) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord server = make_server(50, "atomic");
    const TunnelRecord tunnel = make_tunnel(50, server.id, 6'200, "atomic");
    auto transaction = (*repository)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    ASSERT_TRUE((*repository)->servers().create(server, *transaction));
    ASSERT_TRUE((*repository)->tunnels().create(tunnel, *transaction));
    ASSERT_TRUE(transaction->commit());

    EXPECT_TRUE((*repository)->servers().get_by_id(server.id));
    EXPECT_TRUE((*repository)->tunnels().get_by_id(tunnel.id));
}

TEST(StorageTransactionTest, AbandonmentAndConstraintFailureRollBackAllWrites) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord abandoned = make_server(60, "abandoned");
    {
        auto transaction = (*repository)->begin_transaction();
        ASSERT_TRUE(transaction) << transaction.error();
        ASSERT_TRUE((*repository)->servers().create(abandoned, *transaction));

        const auto nested = (*repository)->begin_transaction();
        ASSERT_FALSE(nested);
        EXPECT_EQ(nested.error().code(), common::ErrorCode::invalid_argument);
    }
    EXPECT_EQ((*repository)->servers().get_by_id(abandoned.id).error().code(),
              common::ErrorCode::not_found);

    const ServerRecord first = make_server(61, "duplicate-name");
    ServerRecord duplicate = make_server(62, "duplicate-name");
    auto transaction = (*repository)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    ASSERT_TRUE((*repository)->servers().create(first, *transaction));
    const auto conflict = (*repository)->servers().create(duplicate, *transaction);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code(), common::ErrorCode::already_exists);
    EXPECT_TRUE(transaction->failed());

    const auto committed = transaction->commit();
    ASSERT_FALSE(committed);
    EXPECT_EQ(committed.error().code(), common::ErrorCode::already_exists);
    EXPECT_EQ((*repository)->servers().get_by_id(first.id).error().code(),
              common::ErrorCode::not_found);
}

TEST(StorageTransactionTest, OtherConnectionCannotSeeUncommittedWrites) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();
    test::NativeSqliteDatabase observer{temporary.path()};

    const ServerRecord server = make_server(70, "isolated");
    auto transaction = (*repository)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    ASSERT_TRUE((*repository)->servers().create(server, *transaction));

    EXPECT_EQ(observer.query_int64("SELECT COUNT(*) FROM servers"), 0);
    ASSERT_TRUE(transaction->commit());
    EXPECT_EQ(observer.query_int64("SELECT COUNT(*) FROM servers"), 1);
}

TEST(StorageTransactionTest, SerializesConcurrentWritesOnOneConnection) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    constexpr std::size_t kWorkerCount = 8;
    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkerCount);
    for (std::size_t index = 0; index < kWorkerCount; ++index) {
        workers.emplace_back([&repository, &failures, index] {
            auto created =
                (*repository)
                    ->servers()
                    .create(make_server(100U + index, "concurrent-" + std::to_string(index)));
            if (!created) {
                failures.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0U);
    const auto servers = (*repository)->servers().list();
    ASSERT_TRUE(servers) << servers.error();
    EXPECT_EQ(servers->size(), kWorkerCount);
}

TEST(StorageRepositoryInjectionTest, TriggerFailuresRollBackEveryMutation) {
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("CREATE TRIGGER reject_server_insert BEFORE INSERT ON servers BEGIN "
                         "SELECT RAISE(ABORT, 'injected server insert'); END");
        const auto created = (*repository)->servers().create(make_server(110, "insert"));
        ASSERT_FALSE(created);
        EXPECT_EQ(created.error().code(), common::ErrorCode::invalid_argument);
    }
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        ServerRecord server = make_server(111, "update");
        ASSERT_TRUE((*repository)->servers().create(server));
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("CREATE TRIGGER reject_server_update BEFORE UPDATE ON servers BEGIN "
                         "SELECT RAISE(ABORT, 'injected server update'); END");
        server.endpoint = make_endpoint("new.example.com:2444");
        ++server.updated_at_unix_ms;
        ++server.config_revision;
        const auto updated = (*repository)->servers().update(server);
        ASSERT_FALSE(updated);
        EXPECT_EQ(updated.error().code(), common::ErrorCode::invalid_argument);
    }
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const ServerRecord server = make_server(112, "mark-children");
        const TunnelRecord tunnel = make_tunnel(112, server.id, 6'300, "child");
        ASSERT_TRUE((*repository)->servers().create(server));
        ASSERT_TRUE((*repository)->tunnels().create(tunnel));
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("CREATE TRIGGER reject_child_tombstone BEFORE UPDATE OF desired_state ON "
                         "tunnels BEGIN SELECT RAISE(ABORT, 'injected child tombstone'); END");
        const auto marked = (*repository)->servers().mark_removed(
            server.id, std::max(server.updated_at_unix_ms, tunnel.updated_at_unix_ms) + 100);
        ASSERT_FALSE(marked);
        EXPECT_EQ(marked.error().code(), common::ErrorCode::invalid_argument);
        EXPECT_EQ((*repository)->tunnels().get_by_id(tunnel.id)->desired_state,
                  TunnelDesiredState::active);
    }
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const ServerRecord server = make_server(113, "mark-server");
        ASSERT_TRUE((*repository)->servers().create(server));
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("CREATE TRIGGER reject_server_tombstone BEFORE UPDATE OF desired_state ON "
                         "servers BEGIN SELECT RAISE(ABORT, 'injected server tombstone'); END");
        const auto marked = (*repository)->servers().mark_removed(
            server.id, server.updated_at_unix_ms + 100);
        ASSERT_FALSE(marked);
        EXPECT_EQ(marked.error().code(), common::ErrorCode::invalid_argument);
        EXPECT_EQ((*repository)->servers().get_by_id(server.id)->desired_state,
                  ServerDesiredState::enabled);
    }
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const ServerRecord server = make_server(114, "erase-server");
        const TunnelRecord tunnel = make_tunnel(114, server.id, 6'310, "child");
        ASSERT_TRUE((*repository)->servers().create(server));
        ASSERT_TRUE((*repository)->tunnels().create(tunnel));
        const std::int64_t tombstone_at = std::max(server.updated_at_unix_ms,
                                                   tunnel.updated_at_unix_ms) + 100;
        ASSERT_TRUE((*repository)->tunnels().mark_removed(tunnel.id, tombstone_at));
        ASSERT_TRUE((*repository)->servers().mark_removed(server.id, tombstone_at + 100));
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("CREATE TRIGGER reject_server_delete BEFORE DELETE ON servers BEGIN "
                         "SELECT RAISE(ABORT, 'injected server erase'); END");
        const auto erased = (*repository)->servers().erase(server.id);
        ASSERT_FALSE(erased);
        EXPECT_EQ(erased.error().code(), common::ErrorCode::invalid_argument);
        EXPECT_TRUE((*repository)->servers().get_by_id(server.id));
    }
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const ServerRecord server = make_server(115, "tunnel-mutations");
        TunnelRecord tunnel = make_tunnel(115, server.id, 6'320, "tunnel");
        ASSERT_TRUE((*repository)->servers().create(server));
        {
            test::NativeSqliteDatabase injector{temporary.path()};
            injector.execute("CREATE TRIGGER reject_tunnel_insert BEFORE INSERT ON tunnels BEGIN "
                             "SELECT RAISE(ABORT, 'injected tunnel insert'); END");
            const auto created = (*repository)->tunnels().create(tunnel);
            ASSERT_FALSE(created);
            EXPECT_EQ(created.error().code(), common::ErrorCode::invalid_argument);
            injector.execute("DROP TRIGGER reject_tunnel_insert");
        }
        ASSERT_TRUE((*repository)->tunnels().create(tunnel));
        {
            test::NativeSqliteDatabase injector{temporary.path()};
            injector.execute("CREATE TRIGGER reject_tunnel_update BEFORE UPDATE ON tunnels BEGIN "
                             "SELECT RAISE(ABORT, 'injected tunnel update'); END");
            tunnel.local_endpoint = make_endpoint("127.0.0.1:2222");
            ++tunnel.updated_at_unix_ms;
            ++tunnel.config_revision;
            const auto updated = (*repository)->tunnels().update(tunnel);
            ASSERT_FALSE(updated);
            EXPECT_EQ(updated.error().code(), common::ErrorCode::invalid_argument);
            injector.execute("DROP TRIGGER reject_tunnel_update");
        }
        {
            test::NativeSqliteDatabase injector{temporary.path()};
            injector.execute(
                "CREATE TRIGGER reject_tunnel_tombstone BEFORE UPDATE OF desired_state ON "
                "tunnels BEGIN SELECT RAISE(ABORT, 'injected tunnel tombstone'); END");
            const auto marked = (*repository)->tunnels().mark_removed(
                tunnel.id, tunnel.updated_at_unix_ms + 100);
            ASSERT_FALSE(marked);
            EXPECT_EQ(marked.error().code(), common::ErrorCode::invalid_argument);
            injector.execute("DROP TRIGGER reject_tunnel_tombstone");
        }
        ASSERT_TRUE(
            (*repository)->tunnels().mark_removed(tunnel.id, tunnel.updated_at_unix_ms + 200));
        {
            test::NativeSqliteDatabase injector{temporary.path()};
            injector.execute("CREATE TRIGGER reject_tunnel_delete BEFORE DELETE ON tunnels BEGIN "
                             "SELECT RAISE(ABORT, 'injected tunnel erase'); END");
            const auto erased = (*repository)->tunnels().erase(tunnel.id);
            ASSERT_FALSE(erased);
            EXPECT_EQ(erased.error().code(), common::ErrorCode::invalid_argument);
            EXPECT_TRUE((*repository)->tunnels().get_by_id(tunnel.id));
        }
    }
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const ServerRecord server = make_server(116, "runtime-transition");
        const TunnelRecord tunnel = make_tunnel(116, server.id, 6'330, "runtime");
        ASSERT_TRUE((*repository)->servers().create(server));
        ASSERT_TRUE((*repository)->tunnels().create(tunnel));
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("CREATE TRIGGER reject_runtime_transition BEFORE UPDATE OF actual_state ON "
                         "tunnels WHEN NEW.actual_state = 'active' BEGIN "
                         "SELECT RAISE(ABORT, 'injected runtime transition'); END");
        const auto transition = (*repository)->tunnels().update_runtime_state_if_revision(
            tunnel.id, server.id, tunnel.config_revision, TunnelActualState::active, std::nullopt,
            tunnel.updated_at_unix_ms + 100, true);
        ASSERT_FALSE(transition);
        EXPECT_EQ(transition.error().code(), common::ErrorCode::invalid_argument);
    }
}

TEST(StorageRepositoryInjectionTest, ReadAndCascadePrepareFailuresAreReported) {
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const ServerRecord server = make_server(117, "read-servers");
        ASSERT_TRUE((*repository)->servers().create(server));
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("DROP TABLE tunnels; DROP TABLE servers; "
                         "CREATE TABLE servers(id TEXT PRIMARY KEY)");

        const auto by_id = (*repository)->servers().get_by_id(server.id);
        ASSERT_FALSE(by_id);
        EXPECT_EQ(by_id.error().code(), common::ErrorCode::database_error);
        const auto by_name = (*repository)->servers().get_by_name("read-servers");
        ASSERT_FALSE(by_name);
        EXPECT_EQ(by_name.error().code(), common::ErrorCode::database_error);
        const auto listed = (*repository)->servers().list();
        ASSERT_FALSE(listed);
        EXPECT_EQ(listed.error().code(), common::ErrorCode::database_error);
    }
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const ServerRecord server = make_server(118, "read-tunnels");
        const TunnelRecord tunnel = make_tunnel(118, server.id, 6'340, "read");
        ASSERT_TRUE((*repository)->servers().create(server));
        ASSERT_TRUE((*repository)->tunnels().create(tunnel));
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("DROP TABLE tunnels; CREATE TABLE tunnels(id TEXT PRIMARY KEY)");

        const auto by_id = (*repository)->tunnels().get_by_id(tunnel.id);
        ASSERT_FALSE(by_id);
        EXPECT_EQ(by_id.error().code(), common::ErrorCode::database_error);
        const auto by_name = (*repository)->tunnels().get_by_name("read");
        ASSERT_FALSE(by_name);
        EXPECT_EQ(by_name.error().code(), common::ErrorCode::database_error);
        const auto listed = (*repository)->tunnels().list();
        ASSERT_FALSE(listed);
        EXPECT_EQ(listed.error().code(), common::ErrorCode::database_error);
        const auto by_server = (*repository)->tunnels().list_by_server(server.id);
        ASSERT_FALSE(by_server);
        EXPECT_EQ(by_server.error().code(), common::ErrorCode::database_error);
        const auto pending = (*repository)->tunnels().mark_active_pending_by_server(
            server.id, std::nullopt, tunnel.updated_at_unix_ms + 100);
        ASSERT_FALSE(pending);
        EXPECT_EQ(pending.error().code(), common::ErrorCode::database_error);
    }
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const ServerRecord server = make_server(119, "cascade-servers");
        const TunnelRecord tunnel = make_tunnel(119, server.id, 6'350, "cascade");
        ASSERT_TRUE((*repository)->servers().create(server));
        ASSERT_TRUE((*repository)->tunnels().create(tunnel));
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("DROP TABLE tunnels");

        const auto marked = (*repository)->servers().mark_removed(
            server.id, std::max(server.updated_at_unix_ms, tunnel.updated_at_unix_ms) + 100);
        ASSERT_FALSE(marked);
        EXPECT_EQ(marked.error().code(), common::ErrorCode::database_error);
    }
    {
        test::TemporaryDatabaseFile temporary;
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const ServerRecord server = make_server(120, "erase-servers");
        const TunnelRecord tunnel = make_tunnel(120, server.id, 6'360, "erase");
        ASSERT_TRUE((*repository)->servers().create(server));
        ASSERT_TRUE((*repository)->tunnels().create(tunnel));
        const std::int64_t tombstone_at = std::max(server.updated_at_unix_ms,
                                                   tunnel.updated_at_unix_ms) + 100;
        ASSERT_TRUE((*repository)->tunnels().mark_removed(tunnel.id, tombstone_at));
        ASSERT_TRUE((*repository)->servers().mark_removed(server.id, tombstone_at + 100));
        test::NativeSqliteDatabase injector{temporary.path()};
        injector.execute("DROP TABLE tunnels");

        const auto erased = (*repository)->servers().erase(server.id);
        ASSERT_FALSE(erased);
        EXPECT_EQ(erased.error().code(), common::ErrorCode::database_error);
    }
}

} // namespace
} // namespace minitun::storage
