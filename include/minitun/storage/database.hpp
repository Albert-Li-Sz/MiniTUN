#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <minitun/common/error.hpp>
#include <minitun/common/result.hpp>
#include <minitun/storage/diagnostics.hpp>

struct sqlite3;

namespace minitun::storage {

inline constexpr std::string_view kDefaultDatabasePath{"/var/lib/minitun/state.db"};
inline constexpr int kCurrentSchemaVersion = 4;
inline constexpr int kDatabaseBusyTimeoutMilliseconds = 5'000;
inline constexpr int kWalAutoCheckpointPages = 1'000;
inline constexpr std::int64_t kWalJournalSizeLimitBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxDatabasePathBytes = 4'096;

class ServerRepository;
class StateRepository;
class TunnelRepository;

class Database;

/// A connection-scoped SQLite write transaction.
///
/// Transactions are thread-affine, hold the connection lock for their entire
/// lifetime, and must not span network or asynchronous operations. An
/// uncommitted transaction is rolled back during destruction.
class Transaction final {
  public:
    ~Transaction() noexcept;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;

    [[nodiscard]] common::Result<void> commit();
    [[nodiscard]] common::Result<void> rollback();
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool failed() const noexcept;

  private:
    friend class Database;
    friend class ServerRepository;
    friend class StateRepository;
    friend class TunnelRepository;

    Transaction(Database& database, std::unique_lock<std::recursive_mutex> lock) noexcept;

    [[nodiscard]] bool belongs_to(const Database& database) const noexcept;
    void mark_failed(common::Error error);
    void rollback_noexcept() noexcept;

    Database* database_{nullptr};
    std::unique_lock<std::recursive_mutex> lock_;
    std::optional<common::Error> failure_;
    bool active_{false};
};

/// Owns one configured SQLite connection and its migration state.
///
/// open() enables WAL, foreign keys, NORMAL synchronous mode, a bounded busy
/// timeout, and applies all supported migrations before returning.
class Database final {
  public:
    [[nodiscard]] static common::Result<std::unique_ptr<Database>> open(std::string_view path);

    ~Database() noexcept;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    [[nodiscard]] common::Result<int> schema_version() const;
    [[nodiscard]] common::Result<Transaction> begin_transaction();
    [[nodiscard]] common::Result<void> checkpoint();
    /// Collects a non-secret health snapshot while the connection is locked.
    [[nodiscard]] common::Result<DatabaseDiagnostics> diagnostics() const;
    /// Creates a consistent SQLite online backup at an atomically installed
    /// destination.  Existing destinations are never overwritten.
    [[nodiscard]] common::Result<void> backup_to(std::string_view destination) const;
    /// Validates restore-source ownership, permissions, integrity, version,
    /// and schema without modifying this database.
    [[nodiscard]] common::Result<void> validate_restore_source(std::string_view source) const;
    /// Restores a validated backup into this connection while holding the
    /// database lock. The per-database copy is atomic and no transaction may
    /// be active.
    [[nodiscard]] common::Result<void> restore_from(std::string_view source) const;
    [[nodiscard]] const std::string& path() const noexcept;

  private:
    friend class Transaction;
    friend class ServerRepository;
    friend class StateRepository;
    friend class TunnelRepository;

    Database(sqlite3* handle, std::string path) noexcept;

    [[nodiscard]] common::Result<void> configure();
    [[nodiscard]] common::Result<void> enable_wal();
    [[nodiscard]] common::Result<void> migrate();

    sqlite3* handle_{nullptr};
    std::string path_;
    mutable std::recursive_mutex mutex_;
    bool transaction_active_{false};
    bool poisoned_{false};
};

} // namespace minitun::storage
