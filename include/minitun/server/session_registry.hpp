#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <minitun/common/result.hpp>

namespace minitun::server {

class SessionRegistry final {
  public:
    explicit SessionRegistry(std::size_t max_clients);

    [[nodiscard]] common::Result<std::uint64_t> open(std::string_view client_id);
    [[nodiscard]] bool is_current(std::string_view client_id,
                                  std::uint64_t generation) const;
    void close(std::string_view client_id, std::uint64_t generation);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t max_clients() const noexcept;

  private:
    std::size_t max_clients_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::uint64_t> generations_;
};

} // namespace minitun::server
