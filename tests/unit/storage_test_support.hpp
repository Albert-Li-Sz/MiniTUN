#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sqlite3.h>

namespace minitun::storage::test {

class TemporaryDatabaseFile final {
  public:
    TemporaryDatabaseFile() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "minitun-storage-test-XXXXXX").string();
        std::vector<char> writable_pattern(pattern.begin(), pattern.end());
        writable_pattern.push_back('\0');

        char* const created = ::mkdtemp(writable_pattern.data());
        if (created == nullptr) {
            throw std::runtime_error(std::string{"mkdtemp failed: "} + std::strerror(errno));
        }
        directory_ = created;
        path_ = directory_ / "state.db";
    }

    ~TemporaryDatabaseFile() noexcept {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    TemporaryDatabaseFile(const TemporaryDatabaseFile&) = delete;
    TemporaryDatabaseFile& operator=(const TemporaryDatabaseFile&) = delete;
    TemporaryDatabaseFile(TemporaryDatabaseFile&&) = delete;
    TemporaryDatabaseFile& operator=(TemporaryDatabaseFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::string path_string() const { return path_.string(); }

  private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

class NativeSqliteDatabase final {
  public:
    explicit NativeSqliteDatabase(const std::filesystem::path& path,
                                  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                                    SQLITE_OPEN_FULLMUTEX) {
        const std::string path_text = path.string();
        const int result = sqlite3_open_v2(path_text.c_str(), &handle_, flags, nullptr);
        if (result != SQLITE_OK) {
            const std::string message =
                handle_ == nullptr ? "unknown SQLite error" : sqlite3_errmsg(handle_);
            close();
            throw std::runtime_error("sqlite3_open_v2 failed: " + message);
        }
        sqlite3_extended_result_codes(handle_, 1);
    }

    ~NativeSqliteDatabase() noexcept { close(); }

    NativeSqliteDatabase(const NativeSqliteDatabase&) = delete;
    NativeSqliteDatabase& operator=(const NativeSqliteDatabase&) = delete;
    NativeSqliteDatabase(NativeSqliteDatabase&&) = delete;
    NativeSqliteDatabase& operator=(NativeSqliteDatabase&&) = delete;

    void close() noexcept {
        if (handle_ != nullptr) {
            sqlite3_close_v2(handle_);
            handle_ = nullptr;
        }
    }

    void execute(const std::string_view sql) {
        char* raw_error = nullptr;
        const std::string owned_sql{sql};
        const int result = sqlite3_exec(handle_, owned_sql.c_str(), nullptr, nullptr, &raw_error);
        if (result == SQLITE_OK) {
            return;
        }

        std::string message = raw_error == nullptr ? sqlite3_errmsg(handle_) : raw_error;
        sqlite3_free(raw_error);
        throw std::runtime_error("sqlite3_exec failed: " + message);
    }

    [[nodiscard]] std::int64_t query_int64(const std::string_view sql) const {
        auto statement = prepare(sql);
        if (sqlite3_step(statement.get()) != SQLITE_ROW ||
            sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
            throw std::runtime_error("SQLite query did not return one integer row");
        }
        const std::int64_t value = sqlite3_column_int64(statement.get(), 0);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error("SQLite integer query returned extra rows");
        }
        return value;
    }

    [[nodiscard]] std::string query_text(const std::string_view sql) const {
        auto statement = prepare(sql);
        if (sqlite3_step(statement.get()) != SQLITE_ROW ||
            sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT) {
            throw std::runtime_error("SQLite query did not return one text row");
        }
        const auto* const text = sqlite3_column_text(statement.get(), 0);
        const int bytes = sqlite3_column_bytes(statement.get(), 0);
        std::string value{reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error("SQLite text query returned extra rows");
        }
        return value;
    }

    [[nodiscard]] sqlite3* handle() const noexcept { return handle_; }

  private:
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

    [[nodiscard]] Statement prepare(const std::string_view sql) const {
        if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("SQLite test query is too large");
        }

        sqlite3_stmt* raw_statement = nullptr;
        const int result = sqlite3_prepare_v2(handle_, sql.data(), static_cast<int>(sql.size()),
                                              &raw_statement, nullptr);
        if (result != SQLITE_OK) {
            throw std::runtime_error(std::string{"sqlite3_prepare_v2 failed: "} +
                                     sqlite3_errmsg(handle_));
        }
        return Statement{raw_statement, sqlite3_finalize};
    }

    sqlite3* handle_{nullptr};
};

[[nodiscard]] inline std::string read_binary_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("failed to open test file for reading");
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

inline void write_binary_file(const std::filesystem::path& path, const std::string_view contents) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error("failed to open test file for writing");
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error("failed to write test file");
    }
}

} // namespace minitun::storage::test
