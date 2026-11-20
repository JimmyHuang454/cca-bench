#ifndef TCP_MORTISE_COPA_H
#define TCP_MORTISE_COPA_H

#include "tcp-congestion-ops.h"

#include <cstdint>
#include <deque>

namespace ns3
{

namespace mortise_detail
{

struct MinMaxSample
{
    uint64_t t = 0;
    uint64_t v = 0;
};

struct MinMaxU64
{
    MinMaxSample s[3];

    uint64_t Get() const;
    uint64_t Reset(uint64_t t, uint64_t meas);
    uint64_t SubwinUpdate(uint64_t win, const MinMaxSample& val);
    uint64_t RunningMin(uint64_t win, uint64_t t, uint64_t meas);
};

struct RttEntry
{
    uint64_t rtt;
    uint64_t time;
};

enum CopaDirection
{
    DIR_NONE = 0,
    DIR_UP = 1,
    DIR_DOWN = 2,
};

enum CopaMode
{
    MODE_DEFAULT = 0,
    MODE_TCPCOOP = 1,
    MODE_LOSS = 2,
};

struct VelocityState
{
    uint64_t velocity = 1;
    CopaDirection direction = DIR_NONE;
    uint64_t num_times_direction_same = 0;
    uint64_t last_recorded_cwnd_bytes = 0;
    uint64_t last_cwnd_record_time = 0;
    uint64_t time_since_direction = 0;

    void Reset();
};

} // namespace mortise_detail

/*
 * 1:1 ns-3 port of official BobAnkh/Mortise algorithm/bpf-kern/mortise_copa.bpf.c
 */
class TcpMortiseCopa : public TcpCongestionOps
{
  public:
    static constexpr uint64_t USEC_PER_SEC_U64 = 1000000ULL;
    static constexpr uint64_t MIN_RTT_WINDOW_US = 10ULL * USEC_PER_SEC_U64;
    static constexpr uint64_t STANDING_RTT_WINDOW_US = 100000ULL;
    static constexpr uint64_t QUANTIZATION_BASE = 1000ULL;
    static constexpr uint64_t REPORT_RTT_INTERVAL = QUANTIZATION_BASE / 2;
    static constexpr uint32_t MIN_CWND_SEGMENT = 4U;

    static TypeId GetTypeId();

    TcpMortiseCopa();
    TcpMortiseCopa(const TcpMortiseCopa& other);
    ~TcpMortiseCopa() override = default;

    std::string GetName() const override;
    bool HasCongControl() const override;
    Ptr<TcpCongestionOps> Fork() override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    void CongControl(Ptr<TcpSocketState> tcb,
                     const TcpRateOps::TcpRateConnection& rc,
                     const TcpRateOps::TcpRateSample& rs) override;

  private:
    void EnsureInit(Ptr<TcpSocketState> tcb);
    void UpdatePacingRateFromRtt(Ptr<TcpSocketState> tcb, uint32_t gainInt);
    void NewRttSample(uint64_t rtt, uint64_t now);
    bool TcpDetected(uint64_t rttMin, uint64_t srtt, uint64_t now) const;
    void ReportMeasurement(uint64_t rttMin, uint64_t srtt, uint64_t now, uint32_t acked, uint32_t lost);
    void ChangeDirection(uint64_t now, mortise_detail::CopaDirection dir, uint32_t cwndBytes);
    void CheckAndUpdateDirection(uint64_t now, uint64_t srtt, uint32_t cwndBytes, uint32_t ackedBytes);
    static uint64_t QuantizeUs(uint64_t valUs, uint32_t clockUs);

    uint32_t m_copaClockUs{1};
    bool m_initialized = false;
    bool m_useStandingRtt = true;
    bool m_isSlowStart = true;
    uint64_t m_deltaParam = 40;
    uint64_t m_defaultParam = 40;
    uint64_t m_maxTime = 10ULL * USEC_PER_SEC_U64;
    mortise_detail::CopaMode m_mode = mortise_detail::MODE_DEFAULT;
    bool m_modeSwitch = true;
    uint32_t m_totalAckedBytes = 0;
    uint32_t m_curNumAcked = 0;
    uint32_t m_curNumLosses = 0;
    uint32_t m_reportAckedBytes = 0;
    uint32_t m_reportLostBytes = 0;
    uint64_t m_prevLossCycle = 0;
    uint64_t m_prevLossRate = 0;
    uint64_t m_lastReportTime = 0;
    uint64_t m_bounceIntervals = 0;
    uint64_t m_intervalsCnt = 0;
    bool m_tcpCoopForced = false;
    mortise_detail::MinMaxU64 m_minRtt;
    mortise_detail::MinMaxU64 m_standingRtt;
    std::deque<mortise_detail::RttEntry> m_rttHist;
    mortise_detail::VelocityState m_vel;
};

} // namespace ns3

#endif // TCP_MORTISE_COPA_H
