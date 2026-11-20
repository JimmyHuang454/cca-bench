#include "tcp-mortise-copa.h"

#include "ns3/data-rate.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <limits>

namespace ns3
{

namespace mortise_detail
{

uint64_t
MinMaxU64::Get() const
{
    return s[0].v;
}

uint64_t
MinMaxU64::Reset(uint64_t t, uint64_t meas)
{
    MinMaxSample val{t, meas};
    s[2] = s[1] = s[0] = val;
    return s[0].v;
}

uint64_t
MinMaxU64::SubwinUpdate(uint64_t win, const MinMaxSample& val)
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

uint64_t
MinMaxU64::RunningMin(uint64_t win, uint64_t t, uint64_t meas)
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

void
VelocityState::Reset()
{
    velocity = 1;
    direction = DIR_NONE;
    num_times_direction_same = 0;
    last_recorded_cwnd_bytes = 0;
    last_cwnd_record_time = 0;
    time_since_direction = 0;
}

} // namespace mortise_detail

NS_OBJECT_ENSURE_REGISTERED(TcpMortiseCopa);

TypeId
TcpMortiseCopa::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpMortiseCopa")
            .SetParent<TcpCongestionOps>()
            .SetGroupName("Internet")
            .AddConstructor<TcpMortiseCopa>()
            .AddAttribute("CopaClockUs",
                          "Timestamp/RTT clock quantization in us (1 = exact us, 1000 = 1ms slow clock)",
                          UintegerValue(1),
                          MakeUintegerAccessor(&TcpMortiseCopa::m_copaClockUs),
                          MakeUintegerChecker<uint32_t>());
    return tid;
}

TcpMortiseCopa::TcpMortiseCopa() = default;

TcpMortiseCopa::TcpMortiseCopa(const TcpMortiseCopa& o)
    : TcpCongestionOps(o),
      m_copaClockUs(o.m_copaClockUs),
      m_initialized(false)
{
}

std::string
TcpMortiseCopa::GetName() const
{
    return "TcpMortiseCopa";
}

bool
TcpMortiseCopa::HasCongControl() const
{
    return true;
}

Ptr<TcpCongestionOps>
TcpMortiseCopa::Fork()
{
    return CopyObject<TcpMortiseCopa>(this);
}

uint32_t
TcpMortiseCopa::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t /*bytesInFlight*/)
{
    return tcb->m_ssThresh.Get();
}

uint64_t
TcpMortiseCopa::QuantizeUs(uint64_t valUs, uint32_t clockUs)
{
    if (clockUs <= 1U || valUs == 0ULL)
    {
        return valUs;
    }
    uint64_t q = (valUs / clockUs) * clockUs;
    return (q > 0ULL) ? q : (uint64_t)clockUs;
}

void
TcpMortiseCopa::EnsureInit(Ptr<TcpSocketState> tcb)
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

void
TcpMortiseCopa::CongControl(Ptr<TcpSocketState> tcb,
                            const TcpRateOps::TcpRateConnection& /*rc*/,
                            const TcpRateOps::TcpRateSample& rs)
{
    EnsureInit(tcb);

    uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);
    uint32_t sndCwndPkts = std::max<uint32_t>(tcb->m_cWnd.Get() / mss, 1U);
    uint32_t cwndBytes = sndCwndPkts * mss;
    uint64_t now = QuantizeUs((uint64_t)Simulator::Now().GetMicroSeconds(), m_copaClockUs);

    Time lrtt = tcb->m_lastRtt.Get();
    Time srttT = tcb->m_srtt.Get();
    int64_t rtt = (lrtt.IsStrictlyPositive() && lrtt != Time::Max())
                      ? (int64_t)QuantizeUs((uint64_t)lrtt.GetMicroSeconds(), m_copaClockUs)
                      : -1;
    uint64_t srtt = (srttT.IsStrictlyPositive() && srttT != Time::Max())
                        ? QuantizeUs((uint64_t)srttT.GetMicroSeconds(), m_copaClockUs)
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

void
TcpMortiseCopa::UpdatePacingRateFromRtt(Ptr<TcpSocketState> tcb, uint32_t gainInt)
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

void
TcpMortiseCopa::NewRttSample(uint64_t rtt, uint64_t now)
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

bool
TcpMortiseCopa::TcpDetected(uint64_t rttMin, uint64_t srtt, uint64_t /*now*/) const
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
    if (minRtt == std::numeric_limits<uint64_t>::max() || maxRtt < rttMin)
    {
        return false;
    }
    uint64_t thresh = rttMin + (maxRtt - rttMin) / 2ULL + 100ULL;
    return minRtt > thresh;
}

void
TcpMortiseCopa::ReportMeasurement(uint64_t rttMin,
                                  uint64_t srtt,
                                  uint64_t now,
                                  uint32_t acked,
                                  uint32_t lost)
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

void
TcpMortiseCopa::ChangeDirection(uint64_t now, mortise_detail::CopaDirection dir, uint32_t cwndBytes)
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

void
TcpMortiseCopa::CheckAndUpdateDirection(uint64_t now,
                                        uint64_t srtt,
                                        uint32_t cwndBytes,
                                        uint32_t ackedBytes)
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

} // namespace ns3
