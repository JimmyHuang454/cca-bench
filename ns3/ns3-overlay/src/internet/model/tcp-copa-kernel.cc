#include "tcp-copa-kernel.h"

#include "ns3/boolean.h"
#include "ns3/data-rate.h"
#include "ns3/integer.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(TcpCopaKernel);

static struct copa_probe_board g_ns3_copa_kernel_board = {};

TypeId
TcpCopaKernel::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpCopaKernel")
            .SetParent<TcpCongestionOps>()
            .SetGroupName("Internet")
            .AddConstructor<TcpCopaKernel>()
            .AddAttribute("Adaptive",
                          "Enable AutoDelta adaptive delta",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpCopaKernel::m_adaptive),
                          MakeBooleanChecker())
            .AddAttribute("V2", "Enable independent-period Copa-Auto v2",
                          BooleanValue(false), MakeBooleanAccessor(&TcpCopaKernel::m_v2),
                          MakeBooleanChecker())
            .AddAttribute("MeasurementRtts", "V2 period length in SRTTs",
                          UintegerValue(16), MakeUintegerAccessor(&TcpCopaKernel::m_measurementRtts),
                          MakeUintegerChecker<uint32_t>(1, COPA_V2_MAX_ROUNDS))
            .AddAttribute("ModeSwitching",
                          "Enable Copa competitive mode switching",
                          BooleanValue(false),
                          MakeBooleanAccessor(&TcpCopaKernel::m_modeSwitching),
                          MakeBooleanChecker())
            .AddAttribute("JitterUs",
                          "Fixed jitter in us (-1 = auto probe, >= 0 = manual fixed j)",
                          IntegerValue(-1),
                          MakeIntegerAccessor(&TcpCopaKernel::m_jitterUs),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("ThetaMilli",
                          "Throughput weight theta*1000 (500 = 0.50)",
                          UintegerValue(500),
                          MakeUintegerAccessor(&TcpCopaKernel::m_thetaMilli),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("InitDeltaQ20",
                          "Initial delta in Q20",
                          UintegerValue(COPA_DELTA_INIT_Q20),
                          MakeUintegerAccessor(&TcpCopaKernel::m_initDeltaQ20),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("CopaClockUs",
                          "Timestamp/RTT clock quantization in us (1 = exact us, 1000 = 1ms slow clock)",
                          UintegerValue(1),
                          MakeUintegerAccessor(&TcpCopaKernel::m_copaClockUs),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("DebugMs",
                          "Emit internal Copa-Auto telemetry to stderr every N ms (0=off)",
                          UintegerValue(0),
                          MakeUintegerAccessor(&TcpCopaKernel::m_debugMs),
                          MakeUintegerChecker<uint32_t>())
            .AddTraceSource("Estimates",
                            "Online AutoDelta estimates (j_s, k_s, rho, r0_s, dq_target_s, delta)",
                            MakeTraceSourceAccessor(&TcpCopaKernel::m_estTrace),
                            "ns3::TcpCopaKernel::EstTracedCallback");
    return tid;
}

TcpCopaKernel::TcpCopaKernel() = default;

TcpCopaKernel::TcpCopaKernel(const TcpCopaKernel& o)
    : TcpCongestionOps(o),
      m_adaptive(o.m_adaptive),
      m_v2(o.m_v2),
      m_measurementRtts(o.m_measurementRtts),
      m_modeSwitching(o.m_modeSwitching),
      m_jitterUs(o.m_jitterUs),
      m_thetaMilli(o.m_thetaMilli),
      m_initDeltaQ20(o.m_initDeltaQ20),
      m_copaClockUs(o.m_copaClockUs),
      m_debugMs(o.m_debugMs),
      m_nextDebugUs(0),
      m_initialized(false)
{
}

std::string
TcpCopaKernel::GetName() const
{
    return "TcpCopaKernel";
}

bool
TcpCopaKernel::HasCongControl() const
{
    return true;
}

Ptr<TcpCongestionOps>
TcpCopaKernel::Fork()
{
    return CopyObject<TcpCopaKernel>(this);
}

uint32_t
TcpCopaKernel::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t /*bytesInFlight*/)
{
    return std::max<uint32_t>(tcb->m_cWnd.Get(), 2U * tcb->m_segmentSize);
}

uint64_t
TcpCopaKernel::QuantizeUs(uint64_t valUs, uint32_t clockUs)
{
    if (clockUs <= 1U || valUs == 0ULL)
    {
        return valUs;
    }
    uint64_t q = (valUs / clockUs) * clockUs;
    return (q > 0ULL) ? q : (uint64_t)clockUs;
}

void
TcpCopaKernel::EnsureInit(Ptr<TcpSocketState> tcb)
{
    tcb->m_pacing = true;
    if (m_initialized)
    {
        return;
    }
    uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);
    uint32_t initPkts = std::max<uint32_t>(tcb->m_cWnd.Get() / mss, 10U);
    struct copa_auto_cfg cfg{};
    cfg.adaptive = m_v2 ? false : m_adaptive;
    cfg.mode_switching = m_v2 ? false : m_modeSwitching;
    cfg.theta_milli = m_thetaMilli;
    cfg.init_delta_q20 = m_initDeltaQ20;
    cfg.jitter_us = m_jitterUs;
    u64 now_us = QuantizeUs((u64)Simulator::Now().GetMicroSeconds(), m_copaClockUs);
    copa_state_init(&m_ca, cfg, &g_ns3_copa_kernel_board, now_us, initPkts);
    if (m_v2) copa_v2_init(&m_v2State, m_measurementRtts);
    m_initialized = true;
}

void
TcpCopaKernel::CongestionStateSet(Ptr<TcpSocketState> tcb,
                                  const TcpSocketState::TcpCongState_t newState)
{
    EnsureInit(tcb);
    if (newState == TcpSocketState::CA_LOSS || newState == TcpSocketState::CA_RECOVERY)
    {
        u64 now_us = QuantizeUs((u64)Simulator::Now().GetMicroSeconds(), m_copaClockUs);
        bool persistent = (newState == TcpSocketState::CA_LOSS);
        if (m_v2 && !persistent) {
            m_ca.slow_start = false;
            m_ca.cwnd_q10 = std::max<u64>(m_ca.cwnd_q10 * 85 / 100, (u64)COPA_MIN_CWND_PKTS << 10);
            copa_reset_velocity(&m_ca);
        } else copa_on_loss(&m_ca, now_us, persistent);
        if (persistent || (!m_ca.competitive && (m_ca.cfg.adaptive || m_v2)))
        {
            uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);
            u32 cwnd_pkts = std::max<u32>((u32)(m_ca.cwnd_q10 >> 10), COPA_MIN_CWND_PKTS);
            tcb->m_cWnd = cwnd_pkts * mss;
        }
    }
}

void
TcpCopaKernel::CongControl(Ptr<TcpSocketState> tcb,
                           const TcpRateOps::TcpRateConnection& /*rc*/,
                           const TcpRateOps::TcpRateSample& rs)
{
    EnsureInit(tcb);
    u64 now_us = QuantizeUs((u64)Simulator::Now().GetMicroSeconds(), m_copaClockUs);
    uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);

    Time lrtt = tcb->m_lastRtt.Get();
    Time srtt = tcb->m_srtt.Get();
    u64 srtt_us = (srtt.IsStrictlyPositive() && srtt != Time::Max())
                      ? QuantizeUs((u64)srtt.GetMicroSeconds(), m_copaClockUs)
                      : 0ULL;
    u64 rtt_sample_us = lrtt.IsStrictlyPositive()
                            ? QuantizeUs((u64)lrtt.GetMicroSeconds(), m_copaClockUs)
                            : ((m_ca.cfg.adaptive || m_v2) ? 0ULL : srtt_us);
    if (!rtt_sample_us && !m_ca.cfg.adaptive)
    {
        return;
    }
    if (srtt_us == 0)
    {
        srtt_us = rtt_sample_us;
    }

    u32 acked_pkts = rs.m_ackedSacked > 0 ? std::max<u32>(rs.m_ackedSacked / mss, 1U) : 1U;
    if (rs.m_bytesLoss > 0)
    {
        copa_record_loss_pkts(&m_ca, std::max<u32>((u32)rs.m_bytesLoss / mss, 1U));
    }
    u64 rate_bps = rs.m_deliveryRate.GetBitRate() / 8ULL;
    bool app_limited = rs.m_isAppLimited;
    bool low_flight = rs.m_isAppLimited && (rs.m_priorInFlight <= 4U * mss);

    if (rs.m_bytesLoss > 0)
    {
        if (!m_v2) copa_on_loss(&m_ca, now_us, false);
    }

    u32 prev_delta_q20 = m_ca.delta_q20;
    u32 new_cwnd = m_v2 ? copa_v2_on_ack(&m_v2State, &m_ca, now_us, rtt_sample_us, srtt_us,
                                      acked_pkts, mss, rate_bps, app_limited, low_flight) :
                     copa_on_ack(&m_ca, now_us, rtt_sample_us, srtt_us,
                               acked_pkts, mss, rate_bps, app_limited, low_flight);
    new_cwnd = std::max<u32>(new_cwnd, COPA_MIN_CWND_PKTS);
    tcb->m_cWnd = new_cwnd * mss;

    u64 pacing_bps = copa_pacing_rate_bps(&m_ca, mss, now_us);
    if (pacing_bps > 0)
    {
        tcb->m_pacingRate = DataRate(pacing_bps * 8ULL);
    }

    if (m_ca.delta_q20 != prev_delta_q20)
    {
        m_estTrace(m_ca.jitter_us / 1e6,
                   m_ca.ratio_us / 1e6,
                   m_ca.rho_milli / 1000.0,
                   m_ca.base_rtt_us / 1e6,
                   m_ca.dq_target_us / 1e6,
                   (double)m_ca.delta_q20 / 1048576.0);
    }

    if (m_debugMs > 0 && now_us >= m_nextDebugUs)
    {
        m_nextDebugUs = now_us + (u64)m_debugMs * 1000ULL;
        std::cerr << std::fixed << std::setprecision(2)
                  << "t=" << (now_us / 1e6)
                  << " cwnd=" << (m_ca.cwnd_q10 >> 10)
                  << " delta=" << std::setprecision(4) << ((double)m_ca.delta_q20 / 1048576.0)
                  << std::setprecision(1)
                  << " v=" << m_ca.velocity
                  << " ss=" << (m_ca.slow_start ? 1 : 0)
                  << " dp=" << (int)m_ca.drain_phase
                  << " j=" << (m_ca.jitter_us / 1000.0) << "ms"
                  << " k=" << (m_ca.ratio_us / 1000.0) << "ms"
                  << " rho=" << m_ca.rho_milli
                  << " dqt=" << (m_ca.dq_target_us / 1000.0) << "ms"
                  << " dq=" << (m_ca.dq_us / 1000.0) << "ms"
                  << " rmin=" << (m_ca.rtt_min.s[0].v / 1000.0) << "ms"
                  << " srtt=" << (m_ca.srtt_us / 1000.0) << "ms"
                  << " app=" << (app_limited ? 1 : 0)
                  << " bw=" << (rate_bps * 8.0 / 1e6) << "M"
                  << "\n";
    }
}

} // namespace ns3
