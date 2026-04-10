#pragma once

#include "foundation/time.hpp"

#include <algorithm>
#include <cmath>

namespace atlas
{

// ============================================================================
// RttEstimator — Jacobson/Karels RTT and RTO estimation (RFC 6298)
// ============================================================================
//
// Extracted from ReliableUdpChannel so it can be unit-tested independently.
//
// Usage:
//   RttEstimator rtt;
//   rtt.set_nodelay(true);          // optional: tighter RTO bounds
//   rtt.update(measured_sample);    // called each time an ACK is received
//   Duration rto = rtt.rto();       // current retransmit timeout
//   Duration smoothed = rtt.rtt();  // smoothed RTT estimate

class RttEstimator
{
public:
    // Update the RTT estimate with a new measured round-trip sample.
    void update(Duration sample)
    {
        double s = std::chrono::duration<double>(sample).count();
        double r = std::chrono::duration<double>(rtt_).count();
        double v = std::chrono::duration<double>(rtt_var_).count();

        if (r < 1e-9)
        {
            // First sample — initialise directly.
            r = s;
            v = s / 2.0;
        }
        else
        {
            v = v * 0.75 + std::abs(r - s) * 0.25;
            r = r * 0.875 + s * 0.125;
        }

        double rto = r + v * 4.0;
        double min_rto = nodelay_ ? 0.03 : 0.2;  // 30ms in nodelay mode, 200ms normal
        rto = std::clamp(rto, min_rto, 5.0);

        rtt_ = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(r));
        rtt_var_ = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(v));
        rto_ = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(rto));
    }

    // nodelay mode: tighter minimum RTO (30ms vs 200ms) and 1.5x backoff on
    // retransmit (instead of 2x).  Suitable for low-latency LAN connections.
    void set_nodelay(bool enable) { nodelay_ = enable; }
    [[nodiscard]] auto nodelay() const -> bool { return nodelay_; }

    [[nodiscard]] auto rtt() const -> Duration { return rtt_; }
    [[nodiscard]] auto rtt_var() const -> Duration { return rtt_var_; }
    [[nodiscard]] auto rto() const -> Duration { return rto_; }

    // RTO after a retransmit timeout: back off (1.5x in nodelay, 2x otherwise).
    [[nodiscard]] auto backoff_rto() const -> Duration
    {
        double factor = nodelay_ ? 1.5 : 2.0;
        auto backed =
            std::chrono::duration_cast<Duration>(std::chrono::duration<double>(rto_) * factor);
        auto max_rto = std::chrono::seconds(5);
        return backed < max_rto ? backed : max_rto;
    }

private:
    Duration rtt_{Milliseconds(200)};
    Duration rtt_var_{Milliseconds(100)};
    Duration rto_{std::chrono::seconds(1)};
    bool nodelay_{false};
};

}  // namespace atlas
