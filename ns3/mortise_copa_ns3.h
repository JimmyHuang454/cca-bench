/*
 * 1:1 ns-3 port of official BobAnkh/Mortise `algorithm/bpf-kern/mortise_copa.bpf.c`
 * (https://github.com/BobAnkh/Mortise/blob/main/algorithm/bpf-kern/mortise_copa.bpf.c)
 *
 * Preserves the exact state machine, constants, windowed min filters,
 * srtt/2 update cadence, slow-start exit halving, velocity doubling,
 * TCPCoop/Loss mode switching, and 2x pacing rate from Mortise.
 */
#pragma once

#include "ns3/internet-module.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>

namespace ns3
{

namespace mortise_detail
{

struct MinMaxSample
{
    uint64_t t = 0;
    uint64_t v = 0;
};

/* Standard Linux lib/minmax.c 3-sample windowed min filter (u64) */
struct MinMaxU64
{
    MinMaxSample s[3];

    uint64_t Get() const
    {
        return s[0].v;
    }

    uint64_t Reset(uint64_t t, uint64_t meas)
    {
        MinMaxSample val{t, meas};
        s[2] = s[1] = s[0] = val;
        return s[0].v;
    }

    uint64_t SubwinUpdate(uint64_t win, const MinMaxSample& val)
    {
        uint64_t dt = val.t - s[0].t;
        if (dt > win)
        {
            s[0] = s[1];
            s[1] = s[2];
            s[2] = val;
            if (val.t - s[0].t > win)
            {
                s[0] = s[1];
                s[1] = s[2];
                s[2] = val;
            }
        }
        else if (s[1].t == s[0].t && dt > win / 4)
        {
            s[2] = s[1] = val;
        }
        else if (s[2].t == s[1].t && dt > win / 2)
        {
            s[2] = val;
        }
        return s[0].v;
    }

    uint64_t RunningMin(uint64_t win, uint64_t t, uint64_t meas)
    {
        MinMaxSample val{t, meas};
        if (val.v <= s[0].v || val.t - s[2].t > win)
        {
            return Reset(t, meas);
        }
        if (val.v <= s[1].v)
        {
            s[2] = s[1] = val;
        }
        else if (val.v <= s[2].v)
        {
            s[2] = val;
        }
        return SubwinUpdate(win, val);
    }
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

    void Reset()
    {
        velocity = 1;
        direction = DIR_NONE;
        num_times_direction_same = 0;
        last_recorded_cwnd_bytes = 0;
        last_cwnd_record_time = 0;
        time_since_direction = 0;
    }
};

} // namespace mortise_detail

class TcpMortiseCopa : public TcpCongestionOps
{
  public:
    static constexpr uint64_t USEC_PER_SEC_U64 = 1000000ULL;
    static constexpr uint64_t MIN_RTT_WINDOW_US = 10ULL * USEC_PER_SEC_U64;
    static constexpr uint64_t STANDING_RTT_WINDOW_US = 100000ULL;
    static constexpr uint64_t QUANTIZATION_BASE = 1000ULL;
    static constexpr uint64_t REPORT_RTT_INTERVAL = QUANTIZATION_BASE / 2; /* 500 */
    static constexpr uint32_t MIN_CWND_SEGMENT = 4U;

    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::TcpMortiseCopa")
                                .SetParent<TcpCongestionOps>()
                                .SetGroupName("Internet")
                                .AddConstructor<TcpMortiseCopa>();
        return tid;
    }

    TcpMortiseCopa() = default;
    TcpMortiseCopa(const TcpMortiseCopa& o)
        : TcpCongestionOps(o),
          m_initialized(false)
    {
    }
    ~TcpMortiseCopa() override = default;

    std::string GetName() const override
    {
        return "TcpMortiseCopa";
    }

    bool HasCongControl() const override
    {
        return true;
    }

    Ptr<TcpCongestionOps> Fork() override
    {
        return CopyObject<TcpMortiseCopa>(this);
    }

    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t /*bytesInFlight*/) override
    {
        return tcb->m_ssThresh.Get();
    }

    void EnsureInit(Ptr<TcpSocketState> tcb)
    {
        if (m_initialized)
        {
            return;
        }
        tcb->m_ssThresh = std::numeric_limits<uint32_t>::max();
        m_useStandingRtt = true;
        m_isSlowStart = true;
        m_deltaParam = 40;
        m_defaultParam = 40;
        m_maxTime = 10ULL * USEC_PER_SEC_U64;
        m_mode = mortise_detail::MODE_DEFAULT;
        m_modeSwitch = true;
        m_totalAckedBytes = 0;
        m_curNumAcked = 0;
        m_curNumLosses = 0;
        m_reportAckedBytes = 0;
        m_reportLostBytes = 0;
        m_prevLossCycle = 0;
        m_prevLossRate = 0;
        m_lastReportTime = 0;
        m_bounceIntervals = 0;
        m_intervalsCnt = 0;
        m_tcpCoopForced = false;
        m_minRtt.Reset(MIN_RTT_WINDOW_US, 0);
        m_standingRtt.Reset(STANDING_RTT_WINDOW_US, 0);
        m_rttHist.clear();
        m_vel.Reset();
        UpdatePacingRateFromRtt(tcb, 2);
        m_initialized = true;
    }

    void CongControl(Ptr<TcpSocketState> tcb,
                     const TcpRateOps::TcpRateConnection& /*rc*/,
                     const TcpRateOps::TcpRateSample& rs) override
    {
        EnsureInit(tcb);

        uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);
        uint32_t sndCwndPkts = std::max<uint32_t>(tcb->m_cWnd.Get() / mss, 1U);
        uint32_t cwndBytes = sndCwndPkts * mss;
        uint64_t now = (uint64_t)Simulator::Now().GetMicroSeconds();

        Time lrtt = tcb->m_lastRtt.Get();
        Time srttT = tcb->m_srtt.Get();
        int64_t rtt = (lrtt.IsStrictlyPositive() && lrtt != Time::Max())
                          ? (int64_t)lrtt.GetMicroSeconds()
                          : -1;
        uint64_t srtt = (srttT.IsStrictlyPositive() && srttT != Time::Max())
                            ? (uint64_t)srttT.GetMicroSeconds()
                            : (rtt > 0 ? (uint64_t)rtt : 20000ULL);

        uint32_t ackedBytes = (rs.m_ackedSacked > 0) ? (uint32_t)rs.m_ackedSacked : 0U;
        uint32_t lostBytes = (rs.m_bytesLoss > 0) ? (uint32_t)rs.m_bytesLoss : 0U;
        m_reportAckedBytes += ackedBytes;
        m_reportLostBytes += lostBytes;

        if (rtt >= 0)
        {
            if (m_minRtt.Get() == 0)
            {
                m_minRtt.Reset(now, (uint64_t)rtt);
            }
            else
            {
                m_minRtt.RunningMin(MIN_RTT_WINDOW_US, now, (uint64_t)rtt);
            }
            if (m_standingRtt.Get() == 0)
            {
                m_standingRtt.Reset(now, (uint64_t)rtt);
            }
            else
            {
                m_standingRtt.RunningMin(srtt / 2, now, (uint64_t)rtt);
            }
            NewRttSample((uint64_t)rtt, now);
        }

        if (m_isSlowStart)
        {
            uint32_t newCwnd = cwndBytes + ackedBytes;
            uint32_t newPkts = std::max<uint32_t>(newCwnd / mss, MIN_CWND_SEGMENT);
            tcb->m_cWnd = newPkts * mss;
            cwndBytes = newPkts * mss;
        }

        if (!(now > 0 &&
              m_lastReportTime + srtt * REPORT_RTT_INTERVAL / QUANTIZATION_BASE < now))
        {
            return;
        }

        uint64_t minRtt = m_minRtt.Get();
        uint64_t minStandingRtt = m_standingRtt.Get();
        if (minRtt == 0 || minStandingRtt < minRtt)
        {
            return;
        }

        ReportMeasurement(minRtt, srtt, now, m_reportAckedBytes, m_reportLostBytes);

        uint64_t delayUs = 0;
        uint32_t ackedPackets = (m_reportAckedBytes + mss - 1U) / mss;
        if (m_useStandingRtt)
        {
            delayUs = minStandingRtt - minRtt;
        }
        else
        {
            delayUs = (rtt > (int64_t)minRtt) ? ((uint64_t)rtt - minRtt) : 0ULL;
        }

        mortise_detail::CopaDirection oldDirection = m_vel.direction;
        bool increaseCwnd = true;
        if (delayUs == 0)
        {
            increaseCwnd = true;
        }
        else
        {
            uint64_t denom = delayUs * std::max<uint64_t>(m_deltaParam, 1ULL);
            uint64_t targetRate = (uint64_t)mss * USEC_PER_SEC_U64 * QUANTIZATION_BASE / denom;
            if (m_intervalsCnt % (m_bounceIntervals + 1ULL) != 0 && !m_isSlowStart)
            {
                targetRate = targetRate * 1700ULL / QUANTIZATION_BASE;
            }
            uint64_t currentRate = (uint64_t)cwndBytes * USEC_PER_SEC_U64 /
                                   std::max<uint64_t>(minStandingRtt, 1ULL);
            increaseCwnd = (targetRate >= currentRate);
        }

        if (!(increaseCwnd && m_isSlowStart))
        {
            CheckAndUpdateDirection(now, srtt, cwndBytes, m_reportAckedBytes);
        }

        uint64_t changeDenom = std::max<uint64_t>(m_deltaParam, 1ULL) *
                               std::max<uint64_t>(cwndBytes, 1ULL);
        uint64_t changeNum = (uint64_t)ackedPackets * (uint64_t)mss * (uint64_t)mss *
                             m_vel.velocity * QUANTIZATION_BASE;
        uint32_t change = (uint32_t)std::min<uint64_t>(changeNum / changeDenom, cwndBytes);

        if (increaseCwnd)
        {
            if (!m_isSlowStart)
            {
                if (m_vel.direction != mortise_detail::DIR_UP && m_vel.velocity > 1)
                {
                    ChangeDirection(now, mortise_detail::DIR_UP, cwndBytes);
                }
                uint64_t newCwnd = (uint64_t)cwndBytes + change;
                uint32_t newPkts = std::max<uint32_t>((uint32_t)(newCwnd / mss), MIN_CWND_SEGMENT);
                tcb->m_cWnd = newPkts * mss;
            }
        }
        else
        {
            if (m_vel.direction != mortise_detail::DIR_DOWN && m_vel.velocity > 1)
            {
                ChangeDirection(now, mortise_detail::DIR_DOWN, cwndBytes);
            }
            uint32_t newCwnd = (cwndBytes > change) ? (cwndBytes - change) : 0U;
            newCwnd = std::max<uint32_t>(newCwnd, MIN_CWND_SEGMENT * mss);
            if (m_isSlowStart)
            {
                newCwnd = std::min<uint32_t>(newCwnd, cwndBytes >> 1);
                newCwnd = std::max<uint32_t>(newCwnd, MIN_CWND_SEGMENT * mss);
            }
            uint32_t newPkts = std::max<uint32_t>(newCwnd / mss, MIN_CWND_SEGMENT);
            tcb->m_cWnd = newPkts * mss;
            m_isSlowStart = false;
        }

        if (oldDirection == mortise_detail::DIR_DOWN && m_vel.direction == mortise_detail::DIR_UP)
        {
            m_intervalsCnt += 1;
        }

        UpdatePacingRateFromRtt(tcb, 2);
        m_lastReportTime = now;
        m_reportAckedBytes = 0;
        m_reportLostBytes = 0;
        m_standingRtt.Reset(now, 1ULL * USEC_PER_SEC_U64);
        tcb->m_ssThresh = tcb->m_cWnd.Get();
    }

  private:
    void UpdatePacingRateFromRtt(Ptr<TcpSocketState> tcb, uint32_t gainInt)
    {
        uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);
        uint32_t cwndPkts = std::max<uint32_t>(tcb->m_cWnd.Get() / mss, 1U);
        uint64_t bps = 0;
        if (tcb->m_srtt.Get().IsPositive())
        {
            uint64_t rttUs = std::max<uint64_t>((uint64_t)tcb->m_srtt.Get().GetMicroSeconds(), 1ULL);
            uint64_t bytesPerSec = ((uint64_t)cwndPkts * mss * gainInt * USEC_PER_SEC_U64) / rttUs;
            bps = bytesPerSec * 8ULL;
        }
        else
        {
            bps = 125000ULL * gainInt * 8ULL;
        }
        tcb->m_pacingRate = DataRate(std::max<uint64_t>(bps, 100000ULL));
    }

    void NewRttSample(uint64_t rtt, uint64_t now)
    {
        m_rttHist.push_back({rtt, now});
        if (now > m_maxTime && m_rttHist.size() > 1)
        {
            uint64_t target = now - m_maxTime;
            while (m_rttHist.size() > 1 && m_rttHist.front().time < target)
            {
                m_rttHist.pop_front();
            }
        }
        if (m_rttHist.size() > 100000)
        {
            m_rttHist.pop_front();
        }
    }

    bool TcpDetected(uint64_t rttMin, uint64_t srtt, uint64_t now) const
    {
        uint64_t minRtt = std::numeric_limits<uint64_t>::max();
        uint64_t maxRtt = 0;
        if (!m_rttHist.empty())
        {
            uint64_t refTime = m_rttHist.back().time;
            uint64_t span = 10ULL * srtt;
            uint64_t earliest = (refTime > span) ? (refTime - span) : 0ULL;
            for (auto it = m_rttHist.rbegin(); it != m_rttHist.rend(); ++it)
            {
                if (it->time <= earliest)
                {
                    break;
                }
                if (it->rtt > maxRtt)
                {
                    maxRtt = it->rtt;
                }
                if (it->rtt < minRtt)
                {
                    minRtt = it->rtt;
                }
            }
        }
        (void)now;
        if (minRtt == std::numeric_limits<uint64_t>::max() || maxRtt < rttMin)
        {
            return false;
        }
        uint64_t thresh = rttMin + (maxRtt - rttMin) / 2ULL + 100ULL;
        return minRtt > thresh;
    }

    void ReportMeasurement(uint64_t rttMin, uint64_t srtt, uint64_t now, uint32_t acked, uint32_t lost)
    {
        m_curNumAcked += acked;
        m_curNumLosses += lost;

        if (now > m_prevLossCycle + 2ULL * rttMin)
        {
            uint64_t total = (uint64_t)m_curNumLosses + (uint64_t)m_curNumAcked;
            if (total > 0)
            {
                m_prevLossRate = (uint64_t)m_curNumLosses * QUANTIZATION_BASE / total;
            }
            m_curNumLosses = 0;
            m_curNumAcked = 0;
            m_prevLossCycle = now;
        }

        if (m_prevLossRate >= QUANTIZATION_BASE / 30ULL)
        {
            m_mode = mortise_detail::MODE_LOSS;
        }
        else if (TcpDetected(rttMin, srtt, now) || m_tcpCoopForced)
        {
            m_mode = mortise_detail::MODE_TCPCOOP;
        }
        else
        {
            m_mode = mortise_detail::MODE_DEFAULT;
            m_deltaParam = m_defaultParam;
        }

        if (m_mode == mortise_detail::MODE_DEFAULT || !m_modeSwitch)
        {
            m_deltaParam = m_defaultParam;
        }
        else if (m_mode == mortise_detail::MODE_TCPCOOP)
        {
            if (lost > 0)
            {
                m_deltaParam = 2ULL * m_deltaParam;
            }
            else
            {
                m_deltaParam = std::max<uint64_t>(
                    m_deltaParam * QUANTIZATION_BASE / (m_deltaParam + QUANTIZATION_BASE), 1ULL);
            }
            if (m_deltaParam > m_defaultParam)
            {
                m_deltaParam = m_defaultParam;
            }
        }
        else if (m_mode == mortise_detail::MODE_LOSS)
        {
            if (lost > 0)
            {
                m_deltaParam = 2ULL * m_deltaParam;
            }
            if (m_deltaParam > m_defaultParam)
            {
                m_deltaParam = m_defaultParam;
            }
        }
    }

    void ChangeDirection(uint64_t now, mortise_detail::CopaDirection dir, uint32_t cwndBytes)
    {
        if (dir == m_vel.direction)
        {
            return;
        }
        m_vel.direction = dir;
        m_vel.velocity = 1;
        m_vel.time_since_direction = now;
        m_vel.last_recorded_cwnd_bytes = cwndBytes;
    }

    void CheckAndUpdateDirection(uint64_t now, uint64_t srtt, uint32_t cwndBytes, uint32_t ackedBytes)
    {
        if (m_vel.last_cwnd_record_time == 0)
        {
            m_vel.last_cwnd_record_time = now;
            m_vel.last_recorded_cwnd_bytes = cwndBytes;
            return;
        }
        m_totalAckedBytes += ackedBytes;
        if (m_totalAckedBytes >= cwndBytes)
        {
            mortise_detail::CopaDirection dir =
                (cwndBytes > m_vel.last_recorded_cwnd_bytes) ? mortise_detail::DIR_UP
                                                             : mortise_detail::DIR_DOWN;
            if (dir == m_vel.direction && (now - m_vel.time_since_direction > 3ULL * srtt))
            {
                m_vel.velocity = std::min<uint64_t>(2ULL * m_vel.velocity, 1ULL << 20);
            }
            else if (dir != m_vel.direction)
            {
                m_vel.velocity = 1;
                m_vel.time_since_direction = now;
            }
            m_vel.direction = dir;
            m_vel.last_cwnd_record_time = now;
            m_vel.last_recorded_cwnd_bytes = cwndBytes;
            m_totalAckedBytes = 0;
        }
    }

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

NS_OBJECT_ENSURE_REGISTERED(TcpMortiseCopa);

} // namespace ns3
