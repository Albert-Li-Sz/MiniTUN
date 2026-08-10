#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/daemon/credential_keys.hpp>
#include <minitun/storage/credential_store.hpp>

#include "storage_test_support.hpp"

namespace minitun::daemon {
namespace {

class FailingCredentialStore final : public storage::CredentialStore {
  public:
    explicit FailingCredentialStore(storage::CredentialStore& delegate) noexcept
        : delegate_(delegate) {}

    void fail_next_get() noexcept { fail_get_ = true; }
    void fail_next_remove() noexcept { fail_remove_ = true; }

    [[nodiscard]] common::Result<void> put(std::string_view key,
                                           std::string_view secret) override {
        return delegate_.put(key, secret);
    }

    [[nodiscard]] common::Result<common::SecureString>
    get(std::string_view key) const override {
        if (fail_get_) {
            fail_get_ = false;
            return common::Error{common::ErrorCode::database_error,
                                 "injected credential read failure"};
        }
        return delegate_.get(key);
    }

    [[nodiscard]] common::Result<void> remove(std::string_view key) override {
        if (fail_remove_) {
            fail_remove_ = false;
            return common::Error{common::ErrorCode::database_error,
                                 "injected credential removal failure"};
        }
        return delegate_.remove(key);
    }

  private:
    storage::CredentialStore& delegate_;
    mutable bool fail_get_{false};
    bool fail_remove_{false};
};

[[nodiscard]] common::Id require_id(const std::string_view value, const common::IdKind kind) {
    auto parsed = common::Id::parse(value, kind);
    if (!parsed) {
        throw std::runtime_error("invalid deterministic credential-key test ID");
    }
    return std::move(*parsed);
}

[[nodiscard]] common::Endpoint require_endpoint(const std::string_view value) {
    auto parsed = common::Endpoint::parse(value);
    if (!parsed) {
        throw std::runtime_error("invalid deterministic credential-key test endpoint");
    }
    return std::move(*parsed);
}

TEST(CredentialKeysTest, RejectsNonServerIdsAndRotatesBetweenBothSlots) {
    const auto client_id = require_id(
        "client_00000000000000000000000000000001", common::IdKind::client);
    const auto tunnel_id =
        require_id("tun_00000000000000000000000000000001", common::IdKind::tunnel);
    storage::test::TemporaryDatabaseFile temporary;
    auto store = storage::SqliteCredentialStore::open(temporary.path_string());
    ASSERT_TRUE(store) << store.error();

    EXPECT_EQ(cleanup_server_credentials(**store, client_id).error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ(cleanup_server_credentials(**store, tunnel_id).error().code(),
              common::ErrorCode::invalid_argument);

    auto server =
        require_id("srv_00000000000000000000000000000001", common::IdKind::server);
    auto psk_keys = managed_server_credential_keys(server, ServerCredentialKind::psk);
    EXPECT_EQ(psk_keys[0], "server/srv_00000000000000000000000000000001");
    EXPECT_EQ(psk_keys[1], "server/srv_00000000000000000000000000000001/next");

    storage::ServerRecord record{
        .id = server,
        .name = "managed",
        .endpoint = require_endpoint("127.0.0.1:9"),
        .credential_ref = psk_keys[0],
        .desired_state = storage::ServerDesiredState::enabled,
        .actual_state = storage::ServerActualState::disconnected,
        .reconnect_attempt = 0U,
        .created_at_unix_ms = 1,
        .updated_at_unix_ms = 1,
    };
    EXPECT_EQ(next_server_credential_key(record, ServerCredentialKind::psk), psk_keys[1]);
    record.credential_ref = psk_keys[1];
    EXPECT_EQ(next_server_credential_key(record, ServerCredentialKind::psk), psk_keys[0]);

    const auto ca_keys = managed_server_credential_keys(server, ServerCredentialKind::ca_certificate);
    EXPECT_EQ(ca_keys[0], "server/srv_00000000000000000000000000000001/ca");
    EXPECT_EQ(ca_keys[1], "server/srv_00000000000000000000000000000001/ca/next");
}

TEST(CredentialKeysTest, CleanupAttemptsEverySlotAndReportsTheFirstError) {
    storage::test::TemporaryDatabaseFile temporary;
    auto backing = storage::SqliteCredentialStore::open(temporary.path_string());
    ASSERT_TRUE(backing) << backing.error();
    FailingCredentialStore store{**backing};

    const auto server =
        require_id("srv_00000000000000000000000000000002", common::IdKind::server);
    const auto primary = "server/srv_00000000000000000000000000000002";
    const auto secondary = "server/srv_00000000000000000000000000000002/next";
    ASSERT_TRUE(store.put(primary, "one"));
    ASSERT_TRUE(store.put(secondary, "two"));

    store.fail_next_remove();
    const auto cleaned = cleanup_server_credentials(store, server);
    ASSERT_FALSE(cleaned);
    EXPECT_EQ(cleaned.error().code(), common::ErrorCode::database_error);
    EXPECT_TRUE(store.get(primary));
    EXPECT_EQ(store.get(secondary).error().code(), common::ErrorCode::not_found);

    const auto retained = cleanup_server_credentials(
        store, server, std::nullopt, std::string_view{primary});
    ASSERT_TRUE(retained) << retained.error();
    EXPECT_TRUE(store.get(primary));
}

TEST(CredentialKeysTest, CleanupAllKindsAttemptsEachCredentialClass) {
    storage::test::TemporaryDatabaseFile temporary;
    auto backing = storage::SqliteCredentialStore::open(temporary.path_string());
    ASSERT_TRUE(backing) << backing.error();
    FailingCredentialStore store{**backing};

    const auto server =
        require_id("srv_00000000000000000000000000000004", common::IdKind::server);
    const auto psk = managed_server_credential_keys(server, ServerCredentialKind::psk)[0];
    const auto ca = managed_server_credential_keys(server, ServerCredentialKind::ca_certificate)[0];
    const auto certificate =
        managed_server_credential_keys(server, ServerCredentialKind::client_certificate)[0];
    const auto key =
        managed_server_credential_keys(server, ServerCredentialKind::client_private_key)[0];
    for (const auto& slot : {psk, ca, certificate, key}) {
        ASSERT_TRUE(store.put(slot, "material"));
    }

    storage::ServerRecord record{
        .id = server,
        .name = "managed",
        .endpoint = require_endpoint("127.0.0.1:9"),
        .credential_ref = psk,
        .desired_state = storage::ServerDesiredState::removed,
        .actual_state = storage::ServerActualState::disabled,
        .reconnect_attempt = 0U,
        .created_at_unix_ms = 1,
        .updated_at_unix_ms = 1,
        .ca_credential_ref = ca,
        .client_certificate_ref = certificate,
        .client_private_key_ref = key,
    };

    store.fail_next_remove();
    const auto cleaned = cleanup_all_server_credentials(store, record);
    ASSERT_FALSE(cleaned);
    EXPECT_EQ(cleaned.error().code(), common::ErrorCode::database_error);
    EXPECT_TRUE(store.get(psk));
    for (const auto& slot : {ca, certificate, key}) {
        EXPECT_EQ(store.get(slot).error().code(), common::ErrorCode::not_found);
    }
}

} // namespace
} // namespace minitun::daemon
