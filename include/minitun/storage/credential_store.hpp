#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <minitun/common/result.hpp>
#include <minitun/common/secure_string.hpp>

struct sqlite3;

namespace minitun::storage {

inline constexpr std::string_view kDefaultCredentialsPath{
    "/var/lib/minitun/credentials.db"};
inline constexpr std::size_t kMaxCredentialKeyBytes = 256;
inline constexpr std::size_t kMaxCredentialSecretBytes = 64U * 1024U;

class CredentialStore {
  public:
    virtual ~CredentialStore() = default;

    [[nodiscard]] virtual common::Result<void> put(std::string_view key,
                                                   std::string_view secret) = 0;
    [[nodiscard]] virtual common::Result<common::SecureString> get(std::string_view key) const = 0;
    [[nodiscard]] virtual common::Result<void> remove(std::string_view key) = 0;
};

class SqliteCredentialStore final : public CredentialStore {
  public:
    [[nodiscard]] static common::Result<std::unique_ptr<SqliteCredentialStore>>
    open(std::string_view path = kDefaultCredentialsPath);

    ~SqliteCredentialStore() noexcept override;

    SqliteCredentialStore(const SqliteCredentialStore&) = delete;
    SqliteCredentialStore& operator=(const SqliteCredentialStore&) = delete;
    SqliteCredentialStore(SqliteCredentialStore&&) = delete;
    SqliteCredentialStore& operator=(SqliteCredentialStore&&) = delete;

    [[nodiscard]] common::Result<void> put(std::string_view key,
                                           std::string_view secret) override;
    [[nodiscard]] common::Result<common::SecureString> get(std::string_view key) const override;
    [[nodiscard]] common::Result<void> remove(std::string_view key) override;

    [[nodiscard]] const std::string& path() const noexcept;

  private:
    SqliteCredentialStore(sqlite3* handle, std::string path) noexcept;

    [[nodiscard]] common::Result<void> configure();
    [[nodiscard]] common::Result<void> migrate();

    sqlite3* handle_{nullptr};
    std::string path_;
    mutable std::mutex mutex_;
};

} // namespace minitun::storage
