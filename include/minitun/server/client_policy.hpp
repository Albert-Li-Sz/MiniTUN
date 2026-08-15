#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <asio/ip/address.hpp>

#include <minitun/common/port_range.hpp>
#include <minitun/common/result.hpp>
#include <minitun/common/secure_string.hpp>

namespace minitun::server {

enum class ClientCertificateBindingKind : std::uint8_t {
    none,
    sha256,
    san,
};

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct ClientCertificateBinding final {
    ClientCertificateBindingKind kind{ClientCertificateBindingKind::none};
    std::string value{};

    friend bool operator==(const ClientCertificateBinding&,
                           const ClientCertificateBinding&) = default;
};

struct SourceCidr final {
    asio::ip::address network;
    std::uint8_t prefix{0U};

    friend bool operator==(const SourceCidr&, const SourceCidr&) = default;
};

struct ClientPolicy final {
    std::string client_id;
    bool enabled{true};
    std::shared_ptr<const common::SecureString> psk;
    /// Rotation predecessor accepted until previous_psk_valid_until (unix
    /// seconds); both fields are either present together or absent.
    std::shared_ptr<const common::SecureString> previous_psk{nullptr};
    std::optional<std::int64_t> previous_psk_valid_until{std::nullopt};
    std::vector<common::PortRange> allowed_ports;
    std::size_t max_tunnels{0U};
    std::size_t max_connections{0U};
    std::size_t max_idle_workers{0U};
    ClientCertificateBinding certificate;
    /// Optional source-address allowlist; an empty list allows every source.
    std::vector<SourceCidr> allowed_source_cidrs;
    /// Per-source connection rate; zero disables rate limiting.
    std::uint32_t connections_per_minute{0U};

    /// A non-reversible digest used only to identify policy changes on reload.
    std::string revision_fingerprint;
    /// Like revision_fingerprint but excludes PSK material so a pure rotation
    /// does not disturb established sessions.
    std::string session_fingerprint;

    [[nodiscard]] bool allows_port(std::uint16_t port) const noexcept;
    [[nodiscard]] bool allows_source(const asio::ip::address& source) const noexcept;

    /// Returns the predecessor PSK when the rotation grace window is still
    /// open at the given unix time, or nullptr.
    [[nodiscard]] std::shared_ptr<const common::SecureString>
    rotation_psk(std::int64_t unix_seconds_now) const noexcept;
};

/// Bounded per-client per-source connection rate limiter. Entries are keyed
/// by client_id and source address; the table stays bounded by evicting an
/// arbitrary entry when it grows past the cap.
class SourceConnectionLimiter final {
  public:
    SourceConnectionLimiter();
    ~SourceConnectionLimiter();
    SourceConnectionLimiter(const SourceConnectionLimiter&) = delete;
    SourceConnectionLimiter& operator=(const SourceConnectionLimiter&) = delete;
    SourceConnectionLimiter(SourceConnectionLimiter&&) noexcept;
    SourceConnectionLimiter& operator=(SourceConnectionLimiter&&) noexcept;

    /// Returns true when one connection is admitted for this key and rate.
    [[nodiscard]] bool allow(std::string_view client_id,
                             const asio::ip::address& source,
                             std::uint32_t connections_per_minute,
                             std::chrono::steady_clock::time_point now) noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};
/// Source admission: applies the optional CIDR allowlist and, when configured,
/// the per-source connection rate. An empty allowlist and a zero rate leave
/// the source unrestricted.
[[nodiscard]] bool admits_source(const ClientPolicy& policy,
                                 SourceConnectionLimiter& limiter,
                                 std::string_view client_id,
                                 const asio::ip::address& source,
                                 std::chrono::steady_clock::time_point now) noexcept;


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

    /// Returns the serialized policy document currently in effect. The text is
    /// the lossless source for management mutations and round-trips through
    /// replace() unchanged.
    [[nodiscard]] common::Result<std::string> document() const;

    /// Validates a replacement document in memory, atomically writes it to the
    /// configured path, and swaps the snapshot only after the write succeeded.
    /// Returns the stable IDs whose effective policy changed. The file is
    /// never left partially written.
    [[nodiscard]] common::Result<std::vector<std::string>>
    replace(std::string document, const SnapshotValidator& validator = {});

    [[nodiscard]] std::shared_ptr<const ClientPolicySnapshot> snapshot() const noexcept;
    [[nodiscard]] std::shared_ptr<const ClientPolicy> find(std::string_view client_id) const;
    [[nodiscard]] const std::string& config_path() const noexcept;

  private:
    ClientPolicyStore(std::string config_path, ClientPolicyLimits limits,
                      std::shared_ptr<const ClientPolicySnapshot> snapshot,
                      std::string document_text) noexcept;

    [[nodiscard]] static common::Result<std::vector<std::string>>
    compute_changed(const std::shared_ptr<const ClientPolicySnapshot>& previous,
                    const std::shared_ptr<const ClientPolicySnapshot>& replacement);

    std::string config_path_;
    ClientPolicyLimits limits_;
    class AtomicSnapshot;
    std::unique_ptr<AtomicSnapshot> snapshot_;
};

} // namespace minitun::server
