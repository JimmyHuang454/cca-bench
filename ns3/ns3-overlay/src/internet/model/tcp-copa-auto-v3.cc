#include "tcp-copa-auto-v3.h"

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

NS_OBJECT_ENSURE_REGISTERED(TcpCopaAutoV3);

static struct copa_v3_board g_ns3_copa_v3_board = {};

TypeId
TcpCopaAutoV3::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpCopaAutoV3")
            .SetParent<TcpCongestionOps>()
            .SetGroupName("Internet")
            .AddConstructor<TcpCopaAutoV3>()
            .AddAttribute("Variant",
                          "Copa-Auto v3 mode (0 = v3a ProbeBW, 1 = v3b Diffusion, 2 = v3 Hybrid)",
                          UintegerValue(COPA_V3_HYBRID),
                          MakeUintegerAccessor(&TcpCopaAutoV3::m_variant),
                          MakeUintegerChecker<uint32_t>(0, 2))
            .AddAttribute("ModeSwitching",
                          "Enable delay-only Copa competitive mode switching",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpCopaAutoV3::m_modeSwitching),
                          MakeBooleanChecker())
            .AddAttribute("JitterUs",
                          "Fixed jitter in us (-1 = auto PROBE_EMPTY, >= 0 = fixed j)",
                          IntegerValue(-1),
                          MakeIntegerAccessor(&TcpCopaAutoV3::m_jitterUs),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("ThetaMilli",
                          "Throughput weight theta*1000 (500 = 0.50)",
                          UintegerValue(500),
                          MakeUintegerAccessor(&TcpCopaAutoV3::m_thetaMilli),
                          MakeUintegerChecker<uint32_t>(10, 995))
            .AddAttribute("InitDeltaQ20",
                          "Initial delta in Q20",
                          UintegerValue(COPA_V3_DELTA_INIT_Q20),
                          MakeUintegerAccessor(&TcpCopaAutoV3::m_initDeltaQ20),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("CopaClockUs",
                          "Timestamp/RTT clock quantization in us (1 = exact us)",
                          UintegerValue(1),
                          MakeUintegerAccessor(&TcpCopaAutoV3::m_copaClockUs),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("DebugMs",
                          "Emit internal Copa-Auto v3 telemetry to stderr every N ms (0=off)",
                          UintegerValue(0),
                          MakeUintegerAccessor(&TcpCopaAutoV3::m_debugMs),
                          MakeUintegerChecker<uint32_t>())
            .AddTraceSource("Estimates",
                            "Online v3 estimates (j_s, k_s, rho, r0_s, dq_target_s, delta)",
                            MakeTraceSourceAccessor(&TcpCopaAutoV3::m_estTrace),
                            "ns3::TcpCopaAutoV3::EstTracedCallback");
    return tid;
}

TcpCopaAutoV3::TcpCopaAutoV3() = default;

TcpCopaAutoV3::TcpCopaAutoV3(const TcpCopaAutoV3& o)
    : TcpCongestionOps(o),
      m_variant(o.m_variant),
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
TcpCopaAutoV3::GetName() const
{
    if (m_variant == COPA_V3_PROBE_BW)
        return "TcpCopaAutoV3a";
    if (m_variant == COPA_V3_DIFFUSION)
        return "TcpCopaAutoV3b";
    return "TcpCopaAutoV3";
}

bool
TcpCopaAutoV3::HasCongControl() const
{
    return true;
}

Ptr<TcpCongestionOps>
TcpCopaAutoV3::Fork()
{
    return CopyObject<TcpCopaAutoV3>(this);
}

uint32_t
TcpCopaAutoV3::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t /*bytesInFlight*/)
{
    return std::max<uint32_t>(tcb->m_cWnd.Get(), 2U * tcb->m_segmentSize);
}

uint64_t
TcpCopaAutoV3::QuantizeUs(uint64_t valUs, uint32_t clockUs)
{
    if (clockUs <= 1U || valUs == 0ULL)
    {
        return valUs;
    }
    uint64_t q = (valUs / clockUs) * clockUs;
    return (q > 0ULL) ? q : (uint64_t)clockUs;
}

void
TcpCopaAutoV3::EnsureInit(Ptr<TcpSocketState> tcb)
{
    tcb->m_pacing = true;
    if (m_initialized)
    {
        return;
    }
    uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);
    uint32_t initPkts = std::max<uint32_t>(tcb->m_cWnd.Get() / mss, 10U);
    struct copa_v3_cfg cfg{};
    cfg.variant = (m_variant == COPA_V3_PROBE_BW)
                      ? COPA_V3_PROBE_BW
                      : ((m_variant == COPA_V3_DIFFUSION) ? COPA_V3_DIFFUSION : COPA_V3_HYBRID);
    cfg.mode_switching = m_modeSwitching;
    cfg.jitter_us = m_jitterUs;
    cfg.theta_milli = m_thetaMilli;
    cfg.init_delta_q20 = m_initDeltaQ20;
    u64 now_us = QuantizeUs((u64)Simulator::Now().GetMicroSeconds(), m_copaClockUs);
    copa_v3_init(&m_st, cfg, &g_ns3_copa_v3_board, now_us, initPkts);
    m_initialized = true;
}

void
TcpCopaAutoV3::CongestionStateSet(Ptr<TcpSocketState> tcb,
                                  const TcpSocketState::TcpCongState_t newState)
{
    EnsureInit(tcb);
    if (newState == TcpSocketState::CA_LOSS || newState == TcpSocketState::CA_RECOVERY)
    {
        u64 now_us = QuantizeUs((u64)Simulator::Now().GetMicroSeconds(), m_copaClockUs);
        bool persistent = (newState == TcpSocketState::CA_LOSS);
        copa_v3_on_loss(&m_st, now_us, persistent);
        if (persistent)
        {
            uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);
            u32 cwnd_pkts = std::max<u32>((u32)(m_st.cwnd_q10 >> 10), COPA_V3_MIN_CWND_PKTS);
            tcb->m_cWnd = cwnd_pkts * mss;
        }
    }
}

void
TcpCopaAutoV3::CongControl(Ptr<TcpSocketState> tcb,
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
                            : 0ULL;
    if (!rtt_sample_us)
    {
        return;
    }
    if (srtt_us == 0)
    {
        srtt_us = rtt_sample_us;
    }

    u32 acked_pkts = rs.m_ackedSacked > 0 ? std::max<u32>(rs.m_ackedSacked / mss, 1U) : 1U;
    u64 rate_bps = rs.m_deliveryRate.GetBitRate() / 8ULL;
    bool app_limited = rs.m_isAppLimited;
    bool low_flight = rs.m_isAppLimited && (rs.m_priorInFlight <= 4U * mss);

    /* Strict default-mode loss insensitivity: only competitive TCPCOOP reacts to SACK loss */
    if (rs.m_bytesLoss > 0 && m_st.competitive)
    {
        copa_v3_on_loss(&m_st, now_us, false);
    }

    u32 prev_delta_q20 = m_st.delta_q20;
    u32 new_cwnd = copa_v3_on_ack(&m_st, now_us, rtt_sample_us, srtt_us,
                                  acked_pkts, mss, rate_bps, app_limited, low_flight);
    new_cwnd = std::max<u32>(new_cwnd, COPA_V3_MIN_CWND_PKTS);
    tcb->m_cWnd = new_cwnd * mss;

    u64 pacing_bps = copa_v3_pacing_rate_bps(&m_st, mss);
    if (pacing_bps > 0)
    {
        tcb->m_pacingRate = DataRate(pacing_bps * 8ULL);
    }

    if (m_st.delta_q20 != prev_delta_q20)
    {
        m_estTrace(m_st.jitter_us / 1e6,
                   m_st.k_us / 1e6,
                   m_st.rho_milli / 1000.0,
                   m_st.base_rtt_us / 1e6,
                   m_st.dq_target_us / 1e6,
                   m_st.delta_q20 / 1048576.0);
    }

    if (m_debugMs > 0 && now_us >= m_nextDebugUs)
    {
        m_nextDebugUs = now_us + (u64)m_debugMs * 1000ULL;
        const char* tag = (m_st.cfg.variant == COPA_V3_PROBE_BW)
                              ? "v3a"
                              : ((m_st.cfg.variant == COPA_V3_DIFFUSION) ? "v3b" : "v3");
        std::cerr << std::fixed << std::setprecision(3)
                  << tag
                  << " t=" << (now_us / 1e6)
                  << " phase=" << (int)m_st.phase
                  << " comp=" << (int)m_st.competitive
                  << " cwnd=" << new_cwnd
                  << " r0_ms=" << (m_st.base_rtt_us / 1e3)
                  << " j_ms=" << (m_st.jitter_us / 1e3)
                  << " dq_ms=" << (m_st.dq_us / 1e3)
                  << " k_ms=" << (m_st.k_us / 1e3)
                  << " vw_ms=" << (m_st.diff_vw_us / 1e3)
                  << " rho=" << (m_st.rho_milli / 1000.0)
                  << " c=" << (m_st.c_milli / 1000.0)
                  << " mu_mbps=" << (m_st.mu_hat_bps * 8.0 / 1e6)
                  << " tgt_ms=" << (m_st.dq_target_us / 1e3)
                  << " delta=" << (m_st.delta_q20 / 1048576.0)
                  << "\n";
    }
}

} // namespace ns3
