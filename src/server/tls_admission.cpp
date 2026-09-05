#include <minitun/server/tls_admission.hpp>

#include <algorithm>
#include <utility>

namespace minitun::server {

TlsAdmission::TlsAdmission(const std::size_t max_pending, const std::size_t max_pending_per_ip,
                           const std::size_t max_per_second)
    : max_pending_(std::max<std::size_t>(1U, max_pending)),
      max_pending_per_ip_(std::max<std::size_t>(1U, max_pending_per_ip)),
      rate_(static_cast<double>(std::max<std::size_t>(1U, max_per_second))), tokens_(rate_) {}

void TlsAdmission::refill(const Clock::time_point now) {
    if (!last_refill_.has_value()) {
        last_refill_ = now;
    } else if (now > *last_refill_) {
        const auto elapsed = std::chrono::duration<double>(now - *last_refill_).count();
        tokens_ = std::min(rate_, tokens_ + elapsed * rate_);
        last_refill_ = now;
    }
}

TlsAdmission::Clock::duration TlsAdmission::accept_delay(const Clock::time_point now) {
    refill(now);
    // A timer keeps the listener responsive without spinning when all pending
    // slots are occupied. Established sessions continue on their own strands.
    Clock::duration delay =
        pending_ >= max_pending_ ? std::chrono::milliseconds{10} : Clock::duration::zero();
    if (tokens_ < 1.0) {
        delay = std::max(delay, std::chrono::ceil<Clock::duration>(
                                    std::chrono::duration<double>{(1.0 - tokens_) / rate_}));
    }
    return delay;
}

void TlsAdmission::record_accept(const Clock::time_point now) {
    refill(now);
    tokens_ = std::max(0.0, tokens_ - 1.0);
}

bool TlsAdmission::try_acquire(const std::string_view address, const Clock::time_point now) {
    if (address.empty() || pending_ >= max_pending_ || !failures_.allowed(address, now)) {
        return false;
    }
    const auto found = sources_.find(std::string{address});
    if (found != sources_.end()) {
        if (found->second >= max_pending_per_ip_) {
            return false;
        }
        ++found->second;
    } else {
        sources_.emplace(address, 1U);
    }
    ++pending_;
    return true;
}

void TlsAdmission::release(const std::string_view address) {
    const auto found = sources_.find(std::string{address});
    if (found == sources_.end()) {
        return;
    }
    --pending_;
    if (--found->second == 0U) {
        sources_.erase(found);
    }
}

std::optional<std::size_t> TlsAdmission::record_failure(const std::string_view address,
                                                        const Clock::time_point now) {
    failures_.record_failure(address, now);
    if (!last_log_.has_value() || now - *last_log_ >= std::chrono::seconds{5}) {
        last_log_ = now;
        return std::exchange(suppressed_, 0U);
    }
    ++suppressed_;
    return std::nullopt;
}

} // namespace minitun::server
