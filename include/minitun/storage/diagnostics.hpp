#pragma once

#include <cstdint>
#include <string>

namespace minitun::storage {

/// A bounded, non-secret snapshot of a SQLite database's health.
///
/// The structure deliberately omits row contents and credential values so it
/// can safely be returned by a local diagnostic command.  A negative WAL
/// counter means that the database is not using WAL journaling (or that the
/// SQLite build cannot expose the counter for this journal mode).
struct DatabaseDiagnostics final {
    std::string path;
    std::int64_t file_size_bytes{0};
    std::uint32_t file_mode{0};
    std::uint64_t owner_uid{0};
    std::uint64_t device{0};
    std::uint64_t inode{0};

    std::int64_t schema_version{0};
    std::string journal_mode;
    std::string synchronous;
    bool foreign_keys{false};
    bool schema_valid{false};
    bool integrity_ok{false};
    std::string integrity_result;

    std::int64_t page_count{0};
    std::int64_t freelist_count{0};
    std::int64_t wal_log_frames{-1};
    std::int64_t wal_checkpointed_frames{-1};
    std::int64_t wal_size_bytes{0};
};

} // namespace minitun::storage
