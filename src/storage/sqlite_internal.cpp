#include "sqlite_internal.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>

namespace minitun::storage::internal {
namespace {

[[nodiscard]] common::Error corrupt_field(const std::string_view field) {
    return common::Error{common::ErrorCode::database_error,
                         std::string{"database contains an invalid "} + std::string{field}};
}

[[nodiscard]] bool is_utf8_continuation(const unsigned char value) noexcept {
    return (value & 0xc0U) == 0x80U;
}

[[nodiscard]] bool is_valid_utf8(const std::string_view value) noexcept {
    std::size_t offset = 0U;
    while (offset < value.size()) {
        const auto byte = [&value, offset](const std::size_t relative_offset) {
            return static_cast<unsigned char>(value[offset + relative_offset]);
        };

        const unsigned char first = byte(0);
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }

        if (first >= 0xc2U && first <= 0xdfU) {
            if (offset + 1U >= value.size() || !is_utf8_continuation(byte(1))) {
                return false;
            }
            offset += 2U;
            continue;
        }

        if (first >= 0xe0U && first <= 0xefU) {
            if (offset + 2U >= value.size() || !is_utf8_continuation(byte(2))) {
                return false;
            }
            const unsigned char second = byte(1);
            const bool valid_second =
                (first == 0xe0U && second >= 0xa0U && second <= 0xbfU) ||
                (first == 0xedU && second >= 0x80U && second <= 0x9fU) ||
                (((first >= 0xe1U && first <= 0xecU) || (first >= 0xeeU && first <= 0xefU)) &&
                 is_utf8_continuation(second));
            if (!valid_second) {
                return false;
            }
            offset += 3U;
            continue;
        }

        if (first >= 0xf0U && first <= 0xf4U) {
            if (offset + 3U >= value.size() || !is_utf8_continuation(byte(2)) ||
                !is_utf8_continuation(byte(3))) {
                return false;
            }
            const unsigned char second = byte(1);
            const bool valid_second =
                (first == 0xf0U && second >= 0x90U && second <= 0xbfU) ||
                (first == 0xf4U && second >= 0x80U && second <= 0x8fU) ||
                (first >= 0xf1U && first <= 0xf3U && is_utf8_continuation(second));
            if (!valid_second) {
                return false;
            }
            offset += 4U;
            continue;
        }

        return false;
    }

    return true;
}

[[nodiscard]] bool is_known_error_code(const common::ErrorCode code) noexcept {
    if (code == common::ErrorCode::ok) {
        return false;
    }
    const auto parsed = common::error_code_from_string(common::to_string(code));
    return parsed.has_value() && *parsed == code;
}

template <typename Enum, typename Parser>
[[nodiscard]] bool is_known_enum(const Enum value, Parser parser) noexcept {
    const auto parsed = parser(to_string(value));
    return parsed.has_value() && *parsed == value;
}

[[nodiscard]] common::Result<void> validate_optional_text(const std::optional<std::string>& value,
                                                          const std::size_t minimum_bytes,
                                                          const std::size_t maximum_bytes,
                                                          const std::string_view field) {
    if (!value.has_value()) {
        return common::Result<void>::success();
    }
    return validate_text(*value, minimum_bytes, maximum_bytes, field);
}

[[nodiscard]] common::Result<void>
validate_error_fields(const std::optional<common::ErrorCode> error_code,
                      const std::optional<std::string>& error_message) {
    if (error_code.has_value() && !is_known_error_code(*error_code)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "last_error_code must be a known non-ok error code"};
    }
    return validate_optional_text(error_message, 0U, kMaxErrorMessageBytes, "last_error_message");
}

[[nodiscard]] common::Result<common::Endpoint> read_endpoint(const std::string_view text,
                                                             const std::string_view field) {
    auto parsed = common::Endpoint::parse(text);
    if (!parsed || parsed->to_string() != text) {
        return corrupt_field(field);
    }
    return parsed;
}

[[nodiscard]] common::Result<common::Endpoint> read_split_endpoint(const std::string_view host,
                                                                   const std::int64_t port,
                                                                   const std::string_view field) {
    if (port < 1 || port > 65'535) {
        return corrupt_field(field);
    }
    auto parsed = common::Endpoint::parse(endpoint_text(host, static_cast<std::uint16_t>(port)));
    if (!parsed || parsed->host() != host || parsed->port() != static_cast<std::uint16_t>(port)) {
        return corrupt_field(field);
    }
    return parsed;
}

} // namespace

Statement::Statement(sqlite3* database, sqlite3_stmt* statement, std::string operation)
    : database_(database), statement_(statement), operation_(std::move(operation)) {}

common::Result<Statement> Statement::prepare(sqlite3* database, const std::string_view sql,
                                             const std::string_view operation) {
    if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return common::Error{common::ErrorCode::resource_exhausted,
                             "SQLite statement exceeds the platform length limit"};
    }

    sqlite3_stmt* statement = nullptr;
    const int result =
        sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
    if (result != SQLITE_OK) {
        return sqlite_error(database, result, operation);
    }
    return Statement{database, statement, std::string{operation}};
}

Statement::~Statement() noexcept {
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
    }
}

Statement::Statement(Statement&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)),
      statement_(std::exchange(other.statement_, nullptr)),
      operation_(std::move(other.operation_)) {}

common::Result<void> Statement::bind_result(const int result) {
    if (result == SQLITE_OK) {
        return common::Result<void>::success();
    }
    return sqlite_error(database_, result, operation_);
}

common::Result<void> Statement::bind_null(const int index) {
    return bind_result(sqlite3_bind_null(statement_, index));
}

common::Result<void> Statement::bind_text(const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return common::Error{common::ErrorCode::resource_exhausted,
                             "SQLite text value exceeds the platform length limit"};
    }
    return bind_result(sqlite3_bind_text(statement_, index, value.data(),
                                         static_cast<int>(value.size()), SQLITE_TRANSIENT));
}

common::Result<void> Statement::bind_blob(const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return common::Error{common::ErrorCode::resource_exhausted,
                             "SQLite blob value exceeds the platform length limit"};
    }
    return bind_result(sqlite3_bind_blob(statement_, index, value.data(),
                                         static_cast<int>(value.size()), SQLITE_TRANSIENT));
}

common::Result<void> Statement::bind_int64(const int index, const std::int64_t value) {
    return bind_result(sqlite3_bind_int64(statement_, index, value));
}

common::Result<StepResult> Statement::step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
        return StepResult::row;
    }
    if (result == SQLITE_DONE) {
        return StepResult::done;
    }
    return sqlite_error(database_, result, operation_);
}

sqlite3_stmt* Statement::handle() const noexcept { return statement_; }

common::Error sqlite_error(sqlite3* database, const int result, const std::string_view operation) {
    const int extended_result = database == nullptr ? result : sqlite3_extended_errcode(database);
    const int primary_result = extended_result & 0xff;

    common::ErrorCode code = common::ErrorCode::database_error;
    if (extended_result == SQLITE_CONSTRAINT_PRIMARYKEY ||
        extended_result == SQLITE_CONSTRAINT_UNIQUE) {
        code = common::ErrorCode::already_exists;
    } else if (extended_result == SQLITE_CONSTRAINT_FOREIGNKEY) {
        code = common::ErrorCode::not_found;
    } else if (primary_result == SQLITE_CONSTRAINT) {
        code = common::ErrorCode::invalid_argument;
    } else if (primary_result == SQLITE_NOMEM || primary_result == SQLITE_FULL ||
               primary_result == SQLITE_TOOBIG) {
        code = common::ErrorCode::resource_exhausted;
    } else if (primary_result == SQLITE_PERM || primary_result == SQLITE_AUTH ||
               primary_result == SQLITE_READONLY) {
        code = common::ErrorCode::permission_denied;
    }

    std::string message{operation};
    const char* const detail = database == nullptr ? nullptr : sqlite3_errmsg(database);
    if (detail != nullptr && detail[0] != '\0') {
        message.append(": ");
        message.append(detail);
    }
    return common::Error{code, std::move(message)};
}

common::Result<void> execute(sqlite3* database, const std::string_view sql,
                             const std::string_view operation) {
    char* raw_error = nullptr;
    const int result =
        sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &raw_error);
    if (result == SQLITE_OK) {
        return common::Result<void>::success();
    }

    common::Error error = sqlite_error(database, result, operation);
    if (raw_error != nullptr) {
        std::string message{operation};
        message.append(": ");
        message.append(raw_error);
        error = common::Error{error.code(), std::move(message)};
    }
    sqlite3_free(raw_error);
    return error;
}

common::Result<std::int64_t> query_single_int64(sqlite3* database, const std::string_view sql,
                                                const std::string_view operation) {
    auto statement = Statement::prepare(database, sql, operation);
    if (!statement) {
        return std::move(statement).error();
    }
    auto step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != StepResult::row || sqlite3_column_type(statement->handle(), 0) != SQLITE_INTEGER) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{operation} + ": expected one integer row"};
    }
    const std::int64_t value = sqlite3_column_int64(statement->handle(), 0);
    step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != StepResult::done) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{operation} + ": expected exactly one row"};
    }
    return value;
}

common::Result<std::string> query_single_text(sqlite3* database, const std::string_view sql,
                                              const std::string_view operation) {
    auto statement = Statement::prepare(database, sql, operation);
    if (!statement) {
        return std::move(statement).error();
    }
    auto step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != StepResult::row) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{operation} + ": expected one text row"};
    }
    auto value = required_text(statement->handle(), 0, operation);
    if (!value) {
        return std::move(value).error();
    }
    step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != StepResult::done) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{operation} + ": expected exactly one row"};
    }
    return value;
}

common::Result<std::string> required_text(sqlite3_stmt* statement, const int column,
                                          const std::string_view field) {
    if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
        return corrupt_field(field);
    }
    const auto* const value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (value == nullptr || bytes < 0) {
        return corrupt_field(field);
    }
    return std::string{reinterpret_cast<const char*>(value), static_cast<std::size_t>(bytes)};
}

common::Result<std::optional<std::string>> optional_text(sqlite3_stmt* statement, const int column,
                                                         const std::string_view field) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::optional<std::string>{};
    }
    auto value = required_text(statement, column, field);
    if (!value) {
        return std::move(value).error();
    }
    return std::optional<std::string>{std::move(*value)};
}

common::Result<std::int64_t> required_int64(sqlite3_stmt* statement, const int column,
                                            const std::string_view field) {
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
        return corrupt_field(field);
    }
    return sqlite3_column_int64(statement, column);
}

common::Result<std::optional<std::int64_t>>
optional_int64(sqlite3_stmt* statement, const int column, const std::string_view field) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::optional<std::int64_t>{};
    }
    auto value = required_int64(statement, column, field);
    if (!value) {
        return std::move(value).error();
    }
    return std::optional<std::int64_t>{*value};
}

common::Result<void> validate_text(const std::string_view value, const std::size_t minimum_bytes,
                                   const std::size_t maximum_bytes, const std::string_view field) {
    if (value.size() < minimum_bytes || value.size() > maximum_bytes) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{field} + " is outside its byte-length limit"};
    }
    if (value.find('\0') != std::string_view::npos) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{field} + " must not contain NUL bytes"};
    }
    if (!is_valid_utf8(value)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{field} + " must contain valid UTF-8"};
    }
    return common::Result<void>::success();
}

common::Result<void> validate_server_record(const ServerRecord& record) {
    if (record.id.kind() != common::IdKind::server) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "server record ID must use the server ID namespace"};
    }
    if (auto result = validate_optional_text(record.name, 1U, kMaxNameBytes, "server name");
        !result) {
        return result;
    }
    if (auto result = validate_optional_text(record.credential_ref, 1U,
                                             kMaxCredentialReferenceBytes, "credential reference");
        !result) {
        return result;
    }
    if (auto result = validate_optional_text(record.remote_server_id, 1U, kMaxRemoteServerIdBytes,
                                             "remote server ID");
        !result) {
        return result;
    }
    if (!is_known_enum(record.desired_state, server_desired_state_from_string) ||
        !is_known_enum(record.actual_state, server_actual_state_from_string)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "server record contains an unknown state"};
    }
    if (auto result = validate_error_fields(record.last_error_code, record.last_error_message);
        !result) {
        return result;
    }
    if (record.reconnect_attempt >
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "reconnect_attempt exceeds the storage limit"};
    }
    if (record.latency_ms.has_value() &&
        (*record.latency_ms < 0 || *record.latency_ms > std::numeric_limits<std::int32_t>::max())) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "latency_ms exceeds the storage limit"};
    }
    if (record.created_at_unix_ms < 0 || record.updated_at_unix_ms < record.created_at_unix_ms) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "server timestamps must be non-negative and monotonic"};
    }
    return common::Result<void>::success();
}

common::Result<void> validate_tunnel_record(const TunnelRecord& record) {
    if (record.id.kind() != common::IdKind::tunnel ||
        record.server_id.kind() != common::IdKind::server) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel record contains an ID from the wrong namespace"};
    }
    if (auto result = validate_optional_text(record.name, 1U, kMaxNameBytes, "tunnel name");
        !result) {
        return result;
    }
    if (!is_known_enum(record.protocol, tunnel_protocol_from_string) ||
        !is_known_enum(record.desired_state, tunnel_desired_state_from_string) ||
        !is_known_enum(record.actual_state, tunnel_actual_state_from_string)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel record contains an unknown protocol or state"};
    }
    if (record.protocol != TunnelProtocol::tcp) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "stage 2 persistence only supports TCP tunnels"};
    }
    if (auto result = validate_error_fields(record.last_error_code, record.last_error_message);
        !result) {
        return result;
    }
    if (record.created_at_unix_ms < 0 || record.updated_at_unix_ms < record.created_at_unix_ms) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel timestamps must be non-negative and monotonic"};
    }
    return common::Result<void>::success();
}

common::Result<ServerRecord> read_server(sqlite3_stmt* statement) {
    auto id_text = required_text(statement, 0, "servers.id");
    auto name = optional_text(statement, 1, "servers.name");
    auto endpoint_value = required_text(statement, 2, "servers.endpoint");
    auto credential_ref = optional_text(statement, 3, "servers.credential_ref");
    auto remote_server_id = optional_text(statement, 4, "servers.remote_server_id");
    auto desired_text = required_text(statement, 5, "servers.desired_state");
    auto actual_text = required_text(statement, 6, "servers.actual_state");
    auto error_text = optional_text(statement, 7, "servers.last_error_code");
    auto error_message = optional_text(statement, 8, "servers.last_error_message");
    auto reconnect_attempt = required_int64(statement, 9, "servers.reconnect_attempt");
    auto latency = optional_int64(statement, 10, "servers.latency_ms");
    auto created_at = required_int64(statement, 11, "servers.created_at");
    auto updated_at = required_int64(statement, 12, "servers.updated_at");

    if (!id_text || !name || !endpoint_value || !credential_ref || !remote_server_id ||
        !desired_text || !actual_text || !error_text || !error_message || !reconnect_attempt ||
        !latency || !created_at || !updated_at) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains a malformed server row"};
    }

    auto id = common::Id::parse(*id_text, common::IdKind::server);
    auto endpoint = read_endpoint(*endpoint_value, "servers.endpoint");
    const auto desired = server_desired_state_from_string(*desired_text);
    const auto actual = server_actual_state_from_string(*actual_text);
    if (!id || !endpoint || !desired.has_value() || !actual.has_value() || *reconnect_attempt < 0 ||
        *reconnect_attempt > std::numeric_limits<std::int32_t>::max() ||
        (latency->has_value() &&
         (**latency < 0 || **latency > std::numeric_limits<std::int32_t>::max()))) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains an invalid server row"};
    }

    std::optional<common::ErrorCode> error_code;
    if (error_text->has_value()) {
        error_code = common::error_code_from_string(**error_text);
        if (!error_code.has_value() || !is_known_error_code(*error_code)) {
            return corrupt_field("servers.last_error_code");
        }
    }

    ServerRecord record{
        .id = std::move(*id),
        .name = std::move(*name),
        .endpoint = std::move(*endpoint),
        .credential_ref = std::move(*credential_ref),
        .remote_server_id = std::move(*remote_server_id),
        .desired_state = *desired,
        .actual_state = *actual,
        .last_error_code = error_code,
        .last_error_message = std::move(*error_message),
        .reconnect_attempt = static_cast<std::uint32_t>(*reconnect_attempt),
        .latency_ms = std::move(*latency),
        .created_at_unix_ms = *created_at,
        .updated_at_unix_ms = *updated_at,
    };
    auto validated = validate_server_record(record);
    if (!validated) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains a server row outside storage limits"};
    }
    return record;
}

common::Result<TunnelRecord> read_tunnel(sqlite3_stmt* statement) {
    auto id_text = required_text(statement, 0, "tunnels.id");
    auto name = optional_text(statement, 1, "tunnels.name");
    auto server_id_text = required_text(statement, 2, "tunnels.server_id");
    auto protocol_text = required_text(statement, 3, "tunnels.protocol");
    auto local_host = required_text(statement, 4, "tunnels.local_host");
    auto local_port = required_int64(statement, 5, "tunnels.local_port");
    auto remote_host = required_text(statement, 6, "tunnels.remote_host");
    auto remote_port = required_int64(statement, 7, "tunnels.remote_port");
    auto desired_text = required_text(statement, 8, "tunnels.desired_state");
    auto actual_text = required_text(statement, 9, "tunnels.actual_state");
    auto error_text = optional_text(statement, 10, "tunnels.last_error_code");
    auto error_message = optional_text(statement, 11, "tunnels.last_error_message");
    auto created_at = required_int64(statement, 12, "tunnels.created_at");
    auto updated_at = required_int64(statement, 13, "tunnels.updated_at");

    if (!id_text || !name || !server_id_text || !protocol_text || !local_host || !local_port ||
        !remote_host || !remote_port || !desired_text || !actual_text || !error_text ||
        !error_message || !created_at || !updated_at) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains a malformed tunnel row"};
    }

    auto id = common::Id::parse(*id_text, common::IdKind::tunnel);
    auto server_id = common::Id::parse(*server_id_text, common::IdKind::server);
    auto local_endpoint = read_split_endpoint(*local_host, *local_port, "tunnels.local_endpoint");
    auto remote_endpoint =
        read_split_endpoint(*remote_host, *remote_port, "tunnels.remote_endpoint");
    const auto protocol = tunnel_protocol_from_string(*protocol_text);
    const auto desired = tunnel_desired_state_from_string(*desired_text);
    const auto actual = tunnel_actual_state_from_string(*actual_text);
    if (!id || !server_id || !local_endpoint || !remote_endpoint || !protocol.has_value() ||
        !desired.has_value() || !actual.has_value()) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains an invalid tunnel row"};
    }

    std::optional<common::ErrorCode> error_code;
    if (error_text->has_value()) {
        error_code = common::error_code_from_string(**error_text);
        if (!error_code.has_value() || !is_known_error_code(*error_code)) {
            return corrupt_field("tunnels.last_error_code");
        }
    }

    TunnelRecord record{
        .id = std::move(*id),
        .name = std::move(*name),
        .server_id = std::move(*server_id),
        .protocol = *protocol,
        .local_endpoint = std::move(*local_endpoint),
        .remote_endpoint = std::move(*remote_endpoint),
        .desired_state = *desired,
        .actual_state = *actual,
        .last_error_code = error_code,
        .last_error_message = std::move(*error_message),
        .created_at_unix_ms = *created_at,
        .updated_at_unix_ms = *updated_at,
    };
    auto validated = validate_tunnel_record(record);
    if (!validated) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains a tunnel row outside storage limits"};
    }
    return record;
}

std::string endpoint_text(const std::string_view host, const std::uint16_t port) {
    if (host.find(':') != std::string_view::npos) {
        return '[' + std::string{host} + "]:" + std::to_string(port);
    }
    return std::string{host} + ':' + std::to_string(port);
}

} // namespace minitun::storage::internal
