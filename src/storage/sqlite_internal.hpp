#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <sqlite3.h>

#include <minitun/common/error.hpp>
#include <minitun/common/result.hpp>
#include <minitun/storage/models.hpp>

namespace minitun::storage::internal {

enum class StepResult {
    row,
    done,
};

class Statement final {
  public:
    [[nodiscard]] static common::Result<Statement> prepare(sqlite3* database, std::string_view sql,
                                                           std::string_view operation);

    ~Statement() noexcept;

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&&) = delete;

    [[nodiscard]] common::Result<void> bind_null(int index);
    [[nodiscard]] common::Result<void> bind_text(int index, std::string_view value);
    [[nodiscard]] common::Result<void> bind_blob(int index, std::string_view value);
    [[nodiscard]] common::Result<void> bind_int64(int index, std::int64_t value);
    [[nodiscard]] common::Result<StepResult> step();

    [[nodiscard]] sqlite3_stmt* handle() const noexcept;

  private:
    Statement(sqlite3* database, sqlite3_stmt* statement, std::string operation);

    [[nodiscard]] common::Result<void> bind_result(int result);

    sqlite3* database_{nullptr};
    sqlite3_stmt* statement_{nullptr};
    std::string operation_;
};

[[nodiscard]] common::Error sqlite_error(sqlite3* database, int result, std::string_view operation);

[[nodiscard]] common::Result<void> execute(sqlite3* database, std::string_view sql,
                                           std::string_view operation);

/// Performs a bounded sqlite3_backup copy into a private temporary file and
/// atomically installs it at destination.  The caller must serialize access to
/// source_database and must not have an active write transaction.
[[nodiscard]] common::Result<void> backup_database(sqlite3* source_database,
                                                   std::string_view source_path,
                                                   std::string_view destination,
                                                   std::string_view description);

/// Validates a restore source without modifying the destination database.
/// The same path, integrity, and caller-provided schema checks are used by
/// restore_database().
[[nodiscard]] common::Result<void>
validate_restore_source(std::string_view destination_path, std::string_view source,
                        std::string_view description,
                        const std::function<common::Result<void>(sqlite3*)>& source_validator = {});

/// Restores a SQLite database into an already-open connection using the
/// online-backup API. The caller must serialize access to the connection and
/// ensure no transaction is active.
[[nodiscard]] common::Result<void>
restore_database(sqlite3* destination_database, std::string_view destination_path,
                 std::string_view source, std::string_view description,
                 const std::function<common::Result<void>(sqlite3*)>& source_validator = {});

[[nodiscard]] common::Result<std::int64_t>
query_single_int64(sqlite3* database, std::string_view sql, std::string_view operation);

[[nodiscard]] common::Result<std::string> query_single_text(sqlite3* database, std::string_view sql,
                                                            std::string_view operation);

[[nodiscard]] common::Result<std::string> required_text(sqlite3_stmt* statement, int column,
                                                        std::string_view field);

[[nodiscard]] common::Result<std::optional<std::string>>
optional_text(sqlite3_stmt* statement, int column, std::string_view field);

[[nodiscard]] common::Result<std::int64_t> required_int64(sqlite3_stmt* statement, int column,
                                                          std::string_view field);

[[nodiscard]] common::Result<std::optional<std::int64_t>>
optional_int64(sqlite3_stmt* statement, int column, std::string_view field);

[[nodiscard]] common::Result<void> validate_text(std::string_view value, std::size_t minimum_bytes,
                                                 std::size_t maximum_bytes, std::string_view field);

[[nodiscard]] common::Result<void> validate_server_record(const ServerRecord& record);
[[nodiscard]] common::Result<void> validate_tunnel_record(const TunnelRecord& record);

[[nodiscard]] common::Result<ServerRecord> read_server(sqlite3_stmt* statement);
[[nodiscard]] common::Result<TunnelRecord> read_tunnel(sqlite3_stmt* statement);

[[nodiscard]] std::string endpoint_text(std::string_view host, std::uint16_t port);

} // namespace minitun::storage::internal
