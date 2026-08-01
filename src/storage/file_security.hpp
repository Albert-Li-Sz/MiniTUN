#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <minitun/common/result.hpp>

namespace minitun::storage::internal {

class PreparedDatabaseFile final {
  public:
    ~PreparedDatabaseFile() noexcept;

    PreparedDatabaseFile(const PreparedDatabaseFile&) = delete;
    PreparedDatabaseFile& operator=(const PreparedDatabaseFile&) = delete;
    PreparedDatabaseFile(PreparedDatabaseFile&& other) noexcept;
    PreparedDatabaseFile& operator=(PreparedDatabaseFile&&) = delete;

    [[nodiscard]] common::Result<void> verify_path_identity() const;

  private:
    friend common::Result<PreparedDatabaseFile>
    prepare_private_database_file(std::string_view path, std::string_view description);

    PreparedDatabaseFile(int descriptor, std::string path, std::string description,
                         std::uint64_t device, std::uint64_t inode) noexcept;

    int descriptor_{-1};
    std::string path_;
    std::string description_;
    std::uint64_t device_{0U};
    std::uint64_t inode_{0U};
};

[[nodiscard]] common::Result<PreparedDatabaseFile>
prepare_private_database_file(std::string_view path, std::string_view description);

} // namespace minitun::storage::internal
