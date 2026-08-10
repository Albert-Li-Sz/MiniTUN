#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <minitun/common/port_range.hpp>
#include <minitun/common/result.hpp>
#include <minitun/common/secure_string.hpp>

namespace minitun::server {

enum class ClientCertificateBindingKind : std::uint8_t {
    none,
    sha256,
    san,
};

struct ClientCertificateBinding final {
    ClientCertificateBindingKind kind{ClientCertificateBindingKind::none};
    std::string value;

    friend bool operator==(const ClientCertificateBinding&,
                           const ClientCertificateBinding&) = default;
};

struct ClientPolicy final {
    std::string client_id;
    bool enabled{true};
    std::shared_ptr<const common::SecureString> psk;
    std::vector<common::PortRange> allowed_ports;
    std::size_t max_tunnels{0U};
    std::size_t max_connections{0U};
    std::size_t max_idle_workers{0U};
    ClientCertificateBinding certificate;

    /// A non-reversible digest used only to identify policy changes on reload.
    std::string revision_fingerprint;

    [[nodiscard]] bool allows_port(std::uint16_t port) const noexcept;
};

struct ClientPolicyLimits final {
    std::size_t max_clients{1'000U};
    std::size_t max_tunnels_per_client{128U};
    std::size_t max_connections_per_client{10'000U};
    std::size_t max_idle_workers_per_client{32U};
};

class ClientPolicySnapshot final {
  public:
    class Impl;

    explicit ClientPolicySnapshot(std::shared_ptr<const Impl> implementation) noexcept;

    [[nodiscard]] std::shared_ptr<const ClientPolicy> find(std::string_view client_id) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool has_certificate_bindings() const noexcept;

  private:
    friend class ClientPolicyStore;

    std::shared_ptr<const Impl> implementation_;
};

class ClientPolicyStore final {
  public:
    using SnapshotValidator =
        std::function<common::Result<void>(const ClientPolicySnapshot& snapshot)>;
    [[nodiscard]] static common::Result<std::shared_ptr<ClientPolicyStore>>
    open(std::string config_path, ClientPolicyLimits limits);

    ClientPolicyStore(const ClientPolicyStore&) = delete;
    ClientPolicyStore& operator=(const ClientPolicyStore&) = delete;

    /// Parses and validates the complete file before atomically replacing the snapshot.
    /// Returns the stable IDs whose effective policy was added, removed, or changed.
    [[nodiscard]] common::Result<std::vector<std::string>>
    reload(const SnapshotValidator& validator = {});

    [[nodiscard]] std::shared_ptr<const ClientPolicySnapshot> snapshot() const noexcept;
    [[nodiscard]] std::shared_ptr<const ClientPolicy> find(std::string_view client_id) const;
    [[nodiscard]] const std::string& config_path() const noexcept;

  private:
    ClientPolicyStore(std::string config_path, ClientPolicyLimits limits,
                      std::shared_ptr<const ClientPolicySnapshot> snapshot) noexcept;

    std::string config_path_;
    ClientPolicyLimits limits_;
    class AtomicSnapshot;
    std::unique_ptr<AtomicSnapshot> snapshot_;
};

} // namespace minitun::server
