#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <asio/any_io_executor.hpp>

#include <minitun/common/port_range.hpp>
#include <minitun/common/result.hpp>

namespace minitun::server {

struct TunnelBinding final {
    std::string client_id;
    std::uint64_t session_generation{0U};
    std::string tunnel_id;
    std::string bind_host;
    std::uint16_t bind_port{0U};

    friend bool operator==(const TunnelBinding&, const TunnelBinding&) = default;
};

class TunnelRegistry final {
  public:
    TunnelRegistry(asio::any_io_executor executor, common::PortRange allowed_ports,
                   std::size_t max_tunnels_per_client);
    ~TunnelRegistry() noexcept;

    TunnelRegistry(const TunnelRegistry&) = delete;
    TunnelRegistry& operator=(const TunnelRegistry&) = delete;

    [[nodiscard]] common::Result<void> register_tunnel(const TunnelBinding& binding);
    void unregister_tunnel(std::string_view client_id, std::uint64_t session_generation,
                           std::string_view tunnel_id) noexcept;
    void remove_session(std::string_view client_id, std::uint64_t session_generation) noexcept;
    void remove_client(std::string_view client_id) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t client_size(std::string_view client_id) const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace minitun::server
