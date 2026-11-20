/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * BBRv2-style congestion control, see tcp-bbr-v2.h for details.
 */

#include "tcp-bbr-v2.h"

#include "tcp-option-ts.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cstdint>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpBbrV2");
NS_OBJECT_ENSURE_REGISTERED(TcpBbrV2);

const char* const TcpBbrV2::BbrV2ModeName[BBRV2_PROBE_RTT + 1] = {
    "BBRV2_STARTUP",
    "BBRV2_DRAIN",
    "BBRV2_PROBE_BW",
    "BBRV2_PROBE_RTT",
};

const char* const TcpBbrV2::ProbeBwPhaseName[PROBE_REFILL + 1] = {
    "PROBE_UP",
    "PROBE_DOWN",
    "PROBE_CRUISE",
    "PROBE_REFILL",
};

TypeId
TcpBbrV2::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpBbrV2")
            .SetParent<TcpCongestionOps>()
            .AddConstructor<TcpBbrV2>()
            .SetGroupName("Internet")
            .AddAttribute("Stream",
                          "Random number stream (default is set to 4 to align with Linux results)",
                          UintegerValue(4),
                          MakeUintegerAccessor(&TcpBbrV2::SetStream),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("HighGain",
                          "Value of the startup gain",
                          DoubleValue(2.89),
                          MakeDoubleAccessor(&TcpBbrV2::m_highGain),
                          MakeDoubleChecker<double>())
            .AddAttribute("BwWindowLength",
                          "Length of the bandwidth windowed filter, in rounds",
                          UintegerValue(10),
                          MakeUintegerAccessor(&TcpBbrV2::m_bandwidthWindowLength),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RttWindowLength",
                          "Length of the minimum RTT windowed filter",
                          TimeValue(Seconds(10)),
                          MakeTimeAccessor(&TcpBbrV2::m_minRttFilterLen),
                          MakeTimeChecker())
            .AddAttribute("ProbeRttDuration",
                          "Time to be spent in the PROBE_RTT phase",
                          TimeValue(MilliSeconds(200)),
                          MakeTimeAccessor(&TcpBbrV2::m_probeRttDuration),
                          MakeTimeChecker())
            .AddAttribute("LossThreshold",
                          "Loss rate above which the inflight_hi bound is reduced",
                          DoubleValue(0.02),
                          MakeDoubleAccessor(&TcpBbrV2::m_lossThreshold),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("EcnThreshold",
                          "ECN marking rate above which the inflight_hi bound is reduced",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&TcpBbrV2::m_ecnThreshold),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("Beta",
                          "Loss rate above which the congestion window is reduced",
                          DoubleValue(0.3),
                          MakeDoubleAccessor(&TcpBbrV2::m_beta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("LinuxLossResponse",
                          "Follow Linux BBRv2/v3: inflight_hi is only set by excessive loss "
                          "while probing for bandwidth, to max(tx_in_flight, (1 - Beta) * "
                          "target); the lower bounds bw_lo/inflight_lo are cut by Beta once per "
                          "lossy round and reset when a new probe starts; an RTO counts as one "
                          "lossy round; the cwnd is restored when loss recovery ends; ProbeBW "
                          "cycles through DOWN, CRUISE, REFILL and UP with a 2-3 s wait between "
                          "probes and the max bandwidth filter covers two cycles. False keeps "
                          "the original model, where every lossy sample sets inflight_hi to the "
                          "inflight that was not lost",
                          BooleanValue(false),
                          MakeBooleanAccessor(&TcpBbrV2::m_linuxLoss),
                          MakeBooleanChecker())
            .AddTraceSource("MinRtt",
                            "Estimated two-way round-trip propagation delay of the path",
                            MakeTraceSourceAccessor(&TcpBbrV2::m_minRtt),
                            "ns3::TracedValueCallback::Time")
            .AddTraceSource("PacingGain",
                            "The dynamic pacing gain factor",
                            MakeTraceSourceAccessor(&TcpBbrV2::m_pacingGain),
                            "ns3::TracedValueCallback::Double")
            .AddTraceSource("CwndGain",
                            "The dynamic congestion window gain factor",
                            MakeTraceSourceAccessor(&TcpBbrV2::m_cWndGain),
                            "ns3::TracedValueCallback::Double");
    return tid;
}

TcpBbrV2::TcpBbrV2()
    : TcpCongestionOps()
{
    NS_LOG_FUNCTION(this);
    m_uv = CreateObject<UniformRandomVariable>();
}

TcpBbrV2::TcpBbrV2(const TcpBbrV2& sock)
    : TcpCongestionOps(sock),
      m_bandwidthWindowLength(sock.m_bandwidthWindowLength),
      m_pacingGain(sock.m_pacingGain),
      m_cWndGain(sock.m_cWndGain),
      m_highGain(sock.m_highGain),
      m_isPipeFilled(sock.m_isPipeFilled),
      m_minPipeCwnd(sock.m_minPipeCwnd),
      m_roundCount(sock.m_roundCount),
      m_roundStart(sock.m_roundStart),
      m_nextRoundDelivered(sock.m_nextRoundDelivered),
      m_probeRttDuration(sock.m_probeRttDuration),
      m_probeRttDoneStamp(sock.m_probeRttDoneStamp),
      m_probeRttRoundDone(sock.m_probeRttRoundDone),
      m_packetConservation(sock.m_packetConservation),
      m_priorCwnd(sock.m_priorCwnd),
      m_idleRestart(sock.m_idleRestart),
      m_targetCWnd(sock.m_targetCWnd),
      m_fullBandwidth(sock.m_fullBandwidth),
      m_fullBandwidthCount(sock.m_fullBandwidthCount),
      m_minRtt(sock.m_minRtt),
      m_minRttStamp(sock.m_minRttStamp),
      m_minRttFilterLen(sock.m_minRttFilterLen),
      m_minRttExpired(sock.m_minRttExpired),
      m_sendQuantum(sock.m_sendQuantum),
      m_cycleStamp(sock.m_cycleStamp),
      m_inflightHi(sock.m_inflightHi),
      m_inflightLo(sock.m_inflightLo),
      m_inflightLatest(sock.m_inflightLatest),
      m_inflightHiMax(sock.m_inflightHiMax),
      m_lossThreshold(sock.m_lossThreshold),
      m_ecnThreshold(sock.m_ecnThreshold),
      m_beta(sock.m_beta),
      m_roundsSinceLoss(sock.m_roundsSinceLoss),
      m_lossRecovery(sock.m_lossRecovery),
      m_linuxLoss(sock.m_linuxLoss),
      m_bwLoLinux(sock.m_bwLoLinux),
      m_lossInRound(sock.m_lossInRound),
      m_bwLatest(sock.m_bwLatest),
      m_inflightLatestRound(sock.m_inflightLatestRound),
      m_probeDownStamp(sock.m_probeDownStamp),
      m_probeWait(sock.m_probeWait),
      m_roundsSinceProbe(sock.m_roundsSinceProbe),
      m_refillRound(sock.m_refillRound),
      m_cycleCount(sock.m_cycleCount),
      m_isInitialized(sock.m_isInitialized),
      m_uv(sock.m_uv),
      m_delivered(sock.m_delivered)
{
    NS_LOG_FUNCTION(this);
}

TcpBbrV2::~TcpBbrV2()
{
    NS_LOG_FUNCTION(this);
}

void
TcpBbrV2::SetRateOps(Ptr<TcpRateOps> rateOps)
{
    NS_LOG_FUNCTION(this << rateOps);
    m_rateOps = rateOps;
}

void
TcpBbrV2::SetStream(uint32_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_uv->SetStream(stream);
}

void
TcpBbrV2::InitRoundCounting()
{
    NS_LOG_FUNCTION(this);
    m_nextRoundDelivered = 0;
    m_roundStart = false;
    m_roundCount = 0;
}

void
TcpBbrV2::InitFullPipe()
{
    NS_LOG_FUNCTION(this);
    m_isPipeFilled = false;
    m_fullBandwidth = 0;
    m_fullBandwidthCount = 0;
}

void
TcpBbrV2::InitPacingRate(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);

    if (!tcb->m_pacing)
    {
        tcb->m_pacing = true;
    }

    Time rtt;
    if (tcb->m_minRtt != Time::Max())
    {
        rtt = MilliSeconds(std::max<long int>(tcb->m_minRtt.GetMilliSeconds(), 1));
    }
    else
    {
        rtt = MilliSeconds(1);
    }

    m_maxBwFilter = MaxBandwidthFilter_t(m_linuxLoss ? 2 : m_bandwidthWindowLength,
                                         DataRate(tcb->m_cWnd * 8 / rtt.GetSeconds()),
                                         0);
    m_bwLoFilter = WindowedFilter<DataRate, MinFilter<DataRate>, uint32_t, uint32_t>(
        m_bandwidthWindowLength,
        DataRate(0),
        0);
    SetPacingRate(tcb, m_pacingGain);
}

void
TcpBbrV2::SetBbrState(BbrV2Mode_t mode)
{
    NS_LOG_FUNCTION(this << mode);
    NS_LOG_DEBUG(Simulator::Now() << " Changing from " << BbrV2ModeName[m_state] << " to "
                                  << BbrV2ModeName[mode]);
    m_state = mode;
}

void
TcpBbrV2::SetProbeBwPhase(Ptr<TcpSocketState> tcb, ProbeBwPhase_t phase)
{
    NS_LOG_FUNCTION(this << tcb << phase);
    m_phase = phase;
    m_cycleStamp = Simulator::Now();
    switch (phase)
    {
    case PROBE_UP:
        m_pacingGain = 1.25;
        m_cWndGain = 2.0;
        break;
    case PROBE_DOWN:
        m_pacingGain = 0.75;
        m_cWndGain = 2.0;
        break;
    case PROBE_CRUISE:
        m_pacingGain = 1.0;
        m_cWndGain = 2.0;
        break;
    case PROBE_REFILL:
        m_pacingGain = 1.0;
        m_cWndGain = 2.0;
        break;
    }
}

void
TcpBbrV2::EnterStartup()
{
    NS_LOG_FUNCTION(this);
    SetBbrState(BBRV2_STARTUP);
    m_pacingGain = m_highGain;
    m_cWndGain = m_highGain;
}

void
TcpBbrV2::EnterDrain()
{
    NS_LOG_FUNCTION(this);
    SetBbrState(BBRV2_DRAIN);
    m_pacingGain = 1.0 / m_highGain;
    m_cWndGain = m_highGain;
}

void
TcpBbrV2::EnterProbeBW()
{
    NS_LOG_FUNCTION(this);
    SetBbrState(BBRV2_PROBE_BW);
    if (m_linuxLoss)
    {
        StartProbeDown();
        return;
    }
    // Start at a random phase to avoid global synchronization, as BBR does.
    uint32_t start = m_uv->GetInteger(0, 3);
    SetProbeBwPhase(nullptr, static_cast<ProbeBwPhase_t>(start));
}

void
TcpBbrV2::EnterProbeRTT()
{
    NS_LOG_FUNCTION(this);
    SetBbrState(BBRV2_PROBE_RTT);
    m_pacingGain = 1.0;
    m_cWndGain = 1.0;
}

void
TcpBbrV2::ExitProbeRTT()
{
    NS_LOG_FUNCTION(this);
    if (m_linuxLoss)
    {
        ResetLowerBounds();
    }
    if (m_isPipeFilled)
    {
        EnterProbeBW();
    }
    else
    {
        EnterStartup();
    }
}

void
TcpBbrV2::SaveCwnd(Ptr<const TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    const bool inRecovery =
        tcb->m_congState == TcpSocketState::CA_RECOVERY ||
        (m_linuxLoss && tcb->m_congState == TcpSocketState::CA_LOSS);
    if (!inRecovery && m_state != BBRV2_PROBE_RTT)
    {
        m_priorCwnd = tcb->m_cWnd;
    }
    else
    {
        m_priorCwnd = std::max(m_priorCwnd, tcb->m_cWnd.Get());
    }
}

void
TcpBbrV2::RestoreCwnd(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    tcb->m_cWnd = std::max(m_priorCwnd, tcb->m_cWnd.Get());
}

uint32_t
TcpBbrV2::InFlight(Ptr<TcpSocketState> tcb, double gain) const
{
    NS_LOG_FUNCTION(this << tcb << gain);
    if (m_minRtt == Time::Max())
    {
        return tcb->m_initialCWnd * tcb->m_segmentSize;
    }
    double quanta = 3 * m_sendQuantum;
    double estimatedBdp = m_maxBwFilter.GetBest() * m_minRtt / 8.0;
    return static_cast<uint32_t>(gain * estimatedBdp) + quanta + 2 * tcb->m_segmentSize;
}

uint32_t
TcpBbrV2::EffectiveInflightHi() const
{
    return m_inflightHi;
}

uint32_t
TcpBbrV2::TargetCwnd(Ptr<TcpSocketState> tcb, double gain) const
{
    uint32_t target = InFlight(tcb, gain);
    if (m_state != BBRV2_STARTUP && m_inflightHi > 0)
    {
        target = std::min(target, m_inflightHi);
    }
    return std::max(target, m_minPipeCwnd);
}

bool
TcpBbrV2::InLossRecovery() const
{
    return m_lossRecovery;
}

void
TcpBbrV2::UpdateRound(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    if (rs.m_priorDelivered >= m_nextRoundDelivered)
    {
        m_nextRoundDelivered = m_delivered;
        m_roundCount++;
        m_roundStart = true;
        m_packetConservation = false;
    }
    else
    {
        m_roundStart = false;
    }
}

void
TcpBbrV2::UpdateRTprop(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    m_minRttExpired = Simulator::Now() > (m_minRttStamp + m_minRttFilterLen);
    if (tcb->m_lastRtt.Get().IsStrictlyPositive() &&
        (tcb->m_lastRtt <= m_minRtt || m_minRttExpired))
    {
        m_minRtt = tcb->m_lastRtt;
        m_minRttStamp = Simulator::Now();
    }
}

void
TcpBbrV2::UpdateBottleneckBandwidth(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);

    if (rs.m_delivered < 0 || rs.m_interval.IsZero())
    {
        return;
    }

    if (rs.m_deliveryRate >= m_maxBwFilter.GetBest() || !rs.m_isAppLimited)
    {
        m_maxBwFilter.Update(rs.m_deliveryRate, BwFilterTime());
    }
    if (m_bbrVersion == 3 && rs.m_deliveryRate.GetBitRate() > 0)
    {
        m_bwLoFilter.Update(rs.m_deliveryRate, m_roundCount);
    }
}

void
TcpBbrV2::ReduceInflightBounds(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);

    uint32_t prior = std::max<uint32_t>(rs.m_priorInFlight, 1);
    uint32_t lost = rs.m_bytesLoss;
    double lossRate = std::min(1.0, static_cast<double>(lost) / prior);
    uint32_t inflightLatest = (prior > lost) ? (prior - lost) : 0;
    m_inflightLatest = inflightLatest;
    m_roundsSinceLoss = 0;

    if (m_linuxLoss)
    {
        ReduceInflightBoundsLinux(tcb, rs, lossRate, prior, inflightLatest);
        return;
    }

    if (m_state == BBRV2_STARTUP)
    {
        // Loss during startup: the pipe is full. Leave startup and remember the
        // inflight at which the path started dropping.
        m_isPipeFilled = true;
        uint32_t bound = std::max(inflightLatest, m_minPipeCwnd);
        m_inflightHi = bound;
        m_inflightLo = std::max(m_inflightLo, bound);
        return;
    }

    if (lossRate >= m_lossThreshold)
    {
        uint32_t hi = (m_inflightHi == 0) ? inflightLatest : std::min(m_inflightHi, inflightLatest);
        m_inflightHi = std::max(hi, m_minPipeCwnd);
        m_inflightLo = std::max(m_inflightLo, std::max(inflightLatest, m_minPipeCwnd));
    }

    if (lossRate >= m_beta)
    {
        // Severe loss: this is the BBRv2 equivalent of a congestion response.
        m_inflightLo = std::max(m_inflightLo, std::max(inflightLatest, m_minPipeCwnd));
        if (!m_lossRecovery)
        {
            m_lossRecoveryRounds = 0;
        }
        m_lossRecovery = true;
        SaveCwnd(tcb);
    }
}

void
TcpBbrV2::ReduceInflightBoundsLinux(Ptr<TcpSocketState> tcb,
                                    const TcpRateOps::TcpRateSample& rs,
                                    double lossRate,
                                    uint32_t prior,
                                    uint32_t inflightLatest)
{
    NS_LOG_FUNCTION(this << tcb << lossRate << prior << inflightLatest);
    m_lossInRound = true;

    // Upper bound: only a lossy bandwidth probe says the path cannot hold more.
    if (lossRate >= m_lossThreshold && IsProbingBandwidth() && !rs.m_isAppLimited)
    {
        const uint32_t target = InFlight(tcb, 1.0);
        const auto floor = static_cast<uint32_t>(target * (1.0 - m_beta));
        m_inflightHi = std::max({prior, floor, m_minPipeCwnd});
        if (m_state == BBRV2_STARTUP)
        {
            m_isPipeFilled = true;
        }
        else if (m_state == BBRV2_PROBE_BW && m_phase == PROBE_UP)
        {
            StartProbeDown();
        }
    }
}

bool
TcpBbrV2::IsProbingBandwidth() const
{
    return m_state == BBRV2_STARTUP ||
           (m_state == BBRV2_PROBE_BW && (m_phase == PROBE_UP || m_phase == PROBE_REFILL));
}

void
TcpBbrV2::ResetLowerBounds()
{
    m_inflightLo = 0;
    m_bwLoLinux = DataRate(0);
}

void
TcpBbrV2::AdaptLowerBounds(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    if (m_bwLoLinux.GetBitRate() == 0)
    {
        m_bwLoLinux = m_maxBwFilter.GetBest();
    }
    if (m_inflightLo == 0)
    {
        m_inflightLo = tcb->m_cWnd.Get();
    }
    m_bwLoLinux = std::max(m_bwLatest,
                           DataRate(static_cast<uint64_t>(m_bwLoLinux.GetBitRate() * (1.0 - m_beta))));
    m_inflightLo = std::max({m_inflightLatestRound,
                             static_cast<uint32_t>(m_inflightLo * (1.0 - m_beta)),
                             m_minPipeCwnd});
    NS_LOG_DEBUG("Lower bounds: bw_lo " << m_bwLoLinux << " inflight_lo " << m_inflightLo);
}

uint32_t
TcpBbrV2::BwFilterTime() const
{
    return m_linuxLoss ? m_cycleCount : m_roundCount;
}

void
TcpBbrV2::StartProbeDown()
{
    NS_LOG_FUNCTION(this);
    m_phase = PROBE_DOWN;
    m_cycleStamp = Simulator::Now();
    m_probeDownStamp = Simulator::Now();
    m_pacingGain = (m_bbrVersion == 3) ? 0.9 : 0.75;
    m_cWndGain = 2.0;
    m_probeWait = Seconds(2.0 + m_uv->GetValue(0.0, 1.0));
    m_roundsSinceProbe = 0;
    m_lossInRound = false;
    m_bwLatest = DataRate(0);
    m_inflightLatestRound = 0;
}

void
TcpBbrV2::StartProbeCruise()
{
    NS_LOG_FUNCTION(this);
    m_phase = PROBE_CRUISE;
    m_cycleStamp = Simulator::Now();
    m_pacingGain = 1.0;
    m_cWndGain = 2.0;
}

void
TcpBbrV2::StartProbeRefill()
{
    NS_LOG_FUNCTION(this);
    ResetLowerBounds();
    m_phase = PROBE_REFILL;
    m_cycleStamp = Simulator::Now();
    m_pacingGain = 1.0;
    m_cWndGain = 2.0;
    m_refillRound = m_roundCount;
    m_cycleCount++;
}

void
TcpBbrV2::StartProbeUp()
{
    NS_LOG_FUNCTION(this);
    m_phase = PROBE_UP;
    m_cycleStamp = Simulator::Now();
    m_pacingGain = 1.25;
    m_cWndGain = (m_bbrVersion == 3) ? 2.25 : 2.0;
}

bool
TcpBbrV2::IsTimeToProbe(Ptr<TcpSocketState> tcb) const
{
    if (Simulator::Now() - m_probeDownStamp >= m_probeWait)
    {
        return true;
    }
    // Reno coexistence: probe at least every min(BDP in packets, 63) rounds.
    const uint32_t bdpPkts = InFlight(tcb, 1.0) / tcb->m_segmentSize;
    return m_roundsSinceProbe >= std::min<uint32_t>(bdpPkts, 63);
}

void
TcpBbrV2::CheckCyclePhaseLinux(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    const uint32_t inflight = tcb->m_bytesInFlight.Get();
    switch (m_phase)
    {
    case PROBE_DOWN:
        if (IsTimeToProbe(tcb))
        {
            StartProbeRefill();
        }
        else if (inflight <= InFlight(tcb, 1.0) &&
                 (m_inflightHi == 0 || inflight <= m_inflightHi * 85 / 100))
        {
            StartProbeCruise();
        }
        break;
    case PROBE_CRUISE:
        if (IsTimeToProbe(tcb))
        {
            StartProbeRefill();
        }
        break;
    case PROBE_REFILL:
        if (m_roundStart && m_roundCount > m_refillRound)
        {
            StartProbeUp();
        }
        break;
    case PROBE_UP:
        if (Simulator::Now() - m_cycleStamp > m_minRtt && inflight >= InFlight(tcb, 1.25))
        {
            StartProbeDown();
        }
        break;
    }
}

uint32_t
TcpBbrV2::BoundCwndForModel(uint32_t cwnd) const
{
    uint32_t cap = UINT32_MAX;
    if (m_inflightHi > 0 && m_state != BBRV2_STARTUP)
    {
        cap = (m_state == BBRV2_PROBE_BW && m_phase == PROBE_CRUISE) ? m_inflightHi * 85 / 100
                                                                       : m_inflightHi;
    }
    if (m_inflightLo > 0)
    {
        cap = std::min(cap, m_inflightLo);
    }
    cap = std::max(cap, m_minPipeCwnd);
    return std::min(cwnd, cap);
}

void
TcpBbrV2::UpdateInflightBounds(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);

    if (m_state == BBRV2_PROBE_RTT)
    {
        return;
    }

    if (m_linuxLoss)
    {
        if (rs.m_bytesLoss > 0)
        {
            ReduceInflightBounds(tcb, rs);
        }
        else
        {
            m_roundsSinceLoss += m_roundStart ? 1 : 0;
        }
        m_bwLatest = std::max(m_bwLatest, rs.m_deliveryRate);
        if (rs.m_delivered > 0)
        {
            m_inflightLatestRound =
                std::max(m_inflightLatestRound, static_cast<uint32_t>(rs.m_delivered));
        }
        if (m_roundStart)
        {
            if (m_lossInRound && !IsProbingBandwidth())
            {
                AdaptLowerBounds(tcb);
            }
            m_lossInRound = false;
            m_bwLatest = DataRate(0);
            m_inflightLatestRound = 0;
            if (m_state == BBRV2_PROBE_BW && (m_phase == PROBE_DOWN || m_phase == PROBE_CRUISE))
            {
                m_roundsSinceProbe++;
            }
            if (m_state == BBRV2_PROBE_BW && m_phase == PROBE_UP && rs.m_bytesLoss == 0)
            {
                uint32_t base = (m_inflightHi == 0)
                                    ? std::max<uint32_t>(tcb->m_bytesInFlight.Get(), m_minPipeCwnd)
                                    : m_inflightHi;
                uint32_t grow = std::max<uint32_t>(2 * tcb->m_segmentSize, base / 4);
                m_inflightHi = std::min<uint32_t>(base + grow, 1u << 30);
            }
        }
        return;
    }

    if (rs.m_bytesLoss > 0)
    {
        ReduceInflightBounds(tcb, rs);
    }
    else
    {
        m_inflightLatest = tcb->m_bytesInFlight.Get();
        m_roundsSinceLoss++;
    }

    if (m_roundStart)
    {
        // Probe upwards only when we are not already limited by inflight_hi.
        if (m_state == BBRV2_PROBE_BW && m_phase == PROBE_UP && rs.m_bytesLoss == 0)
        {
            uint32_t base = (m_inflightHi == 0)
                                ? std::max<uint32_t>(tcb->m_bytesInFlight.Get(), m_minPipeCwnd)
                                : m_inflightHi;
            uint32_t grow = std::max<uint32_t>(2 * tcb->m_segmentSize, base / 4);
            m_inflightHi = std::min<uint32_t>(base + grow, 1u << 30);
            m_inflightHiMax = std::max(m_inflightHiMax, m_inflightHi);
        }

        if (m_linuxLoss && rs.m_bytesLoss == 0 && m_roundsSinceLoss > 1)
        {
            m_inflightLo = 0;
            m_bwLoLinux = DataRate(0);
        }
        // inflight_lo is a soft bound: let it decay once the loss is behind us.
        if (rs.m_bytesLoss == 0 && m_roundsSinceLoss > 1 && m_inflightLo > 0)
        {
            m_inflightLo = m_inflightLo * 3 / 4;
            if (m_roundsSinceLoss > m_bandwidthWindowLength)
            {
                m_inflightLo = 0;
            }
        }

        if (rs.m_bytesLoss == 0 && m_roundsSinceLoss > 0)
        {
            m_lossRecovery = false;
        }
        // Time-box the recovery state: with continuous loss the loss-free test
        // above never fires, and a stale bw_lo/inflight_lo would pin the rate.
        if (m_lossRecovery && ++m_lossRecoveryRounds > 2)
        {
            m_lossRecovery = false;
        }
    }
}

void
TcpBbrV2::CheckFullPipe(const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << rs);
    if (m_isPipeFilled || !m_roundStart || rs.m_isAppLimited)
    {
        return;
    }

    // Consider the pipe full when the bandwidth estimate stops growing.
    if (m_maxBwFilter.GetBest().GetBitRate() >= m_fullBandwidth.GetBitRate() * 1.25)
    {
        m_fullBandwidth = m_maxBwFilter.GetBest();
        m_fullBandwidthCount = 0;
        return;
    }

    m_fullBandwidthCount++;
    if (m_fullBandwidthCount >= 3)
    {
        NS_LOG_DEBUG("Pipe filled");
        m_isPipeFilled = true;
    }
}

void
TcpBbrV2::CheckDrain(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    if (m_state == BBRV2_STARTUP && m_isPipeFilled)
    {
        EnterDrain();
        tcb->m_ssThresh = InFlight(tcb, 1);
    }

    if (m_state == BBRV2_DRAIN && tcb->m_bytesInFlight <= InFlight(tcb, 1))
    {
        EnterProbeBW();
    }
}

bool
TcpBbrV2::IsNextCyclePhase(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs) const
{
    NS_LOG_FUNCTION(this << tcb << rs);

    bool isFullLength = (Simulator::Now() - m_cycleStamp) > m_minRtt;

    switch (m_phase)
    {
    case PROBE_UP:
        // Probe up until the inflight bound is reached, or a round elapsed.
        if (m_inflightHi > 0 && tcb->m_bytesInFlight >= m_inflightHi)
        {
            return true;
        }
        return isFullLength && m_roundStart;
    case PROBE_DOWN:
        // Drain until the inflight_lo bound is reached, or a round elapsed.
        if (m_inflightLo > 0 && tcb->m_bytesInFlight <= std::max(m_inflightLo, m_minPipeCwnd))
        {
            return true;
        }
        return isFullLength && m_roundStart;
    case PROBE_CRUISE:
        return isFullLength && m_roundStart;
    case PROBE_REFILL:
        if (m_roundStart && tcb->m_bytesInFlight >= TargetCwnd(tcb, m_cWndGain))
        {
            return true;
        }
        return isFullLength && m_roundStart;
    }
    return isFullLength && m_roundStart;
}

void
TcpBbrV2::CheckCyclePhase(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    if (m_state != BBRV2_PROBE_BW)
    {
        return;
    }
    if (m_linuxLoss)
    {
        CheckCyclePhaseLinux(tcb, rs);
        return;
    }
    if (IsNextCyclePhase(tcb, rs))
    {
        ProbeBwPhase_t next;
        if (m_bbrVersion == 3)
        {
            // BBRv3 uses a two-phase cycle: probe up, then drain.
            next = (m_phase == PROBE_UP) ? PROBE_DOWN : PROBE_UP;
        }
        else
        {
            switch (m_phase)
            {
            case PROBE_UP:
                next = PROBE_DOWN;
                break;
            case PROBE_DOWN:
                next = PROBE_CRUISE;
                break;
            case PROBE_CRUISE:
                next = PROBE_REFILL;
                break;
            case PROBE_REFILL:
            default:
                next = PROBE_UP;
                break;
            }
        }
        SetProbeBwPhase(tcb, next);
    }
}

void
TcpBbrV2::HandleProbeRTT(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);

    m_rateOps->SetAppLimited(tcb->m_bytesInFlight.Get());

    const uint32_t probeRttCwnd =
        m_linuxLoss ? std::max(InFlight(tcb, 0.5), m_minPipeCwnd) : m_minPipeCwnd;
    if (m_probeRttDoneStamp.IsZero() && tcb->m_bytesInFlight <= probeRttCwnd)
    {
        m_probeRttDoneStamp = Simulator::Now() + m_probeRttDuration;
        m_probeRttRoundDone = false;
        m_nextRoundDelivered = m_delivered;
    }
    else if (!m_probeRttDoneStamp.IsZero())
    {
        if (m_roundStart)
        {
            m_probeRttRoundDone = true;
        }
        if (m_probeRttRoundDone && Simulator::Now() > m_probeRttDoneStamp)
        {
            m_minRttStamp = Simulator::Now();
            RestoreCwnd(tcb);
            ExitProbeRTT();
        }
    }
}

void
TcpBbrV2::CheckProbeRTT(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb);
    if (m_state != BBRV2_PROBE_RTT && m_minRttExpired && !m_idleRestart)
    {
        EnterProbeRTT();
        SaveCwnd(tcb);
        m_probeRttDoneStamp = Seconds(0);
    }

    if (m_state == BBRV2_PROBE_RTT)
    {
        HandleProbeRTT(tcb);
    }

    if (rs.m_delivered)
    {
        m_idleRestart = false;
    }
}

void
TcpBbrV2::HandleRestartFromIdle(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    if (tcb->m_bytesInFlight.Get() == 0U && rs.m_isAppLimited)
    {
        m_idleRestart = true;
        if (m_state == BBRV2_PROBE_BW)
        {
            SetPacingRate(tcb, 1);
        }
    }
}

void
TcpBbrV2::UpdateModelAndState(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    UpdateRound(tcb, rs);
    UpdateBottleneckBandwidth(tcb, rs);
    UpdateInflightBounds(tcb, rs);
    CheckFullPipe(rs);
    CheckCyclePhase(tcb, rs);
    CheckDrain(tcb);
    UpdateRTprop(tcb);
    CheckProbeRTT(tcb, rs);
    HandleRestartFromIdle(tcb, rs);
}

void
TcpBbrV2::SetPacingRate(Ptr<TcpSocketState> tcb, double gain)
{
    NS_LOG_FUNCTION(this << tcb << gain);
    DataRate rate(gain * m_maxBwFilter.GetBest().GetBitRate());
    rate *= 0.99; // one percent margin, as in the reference implementations
    rate = std::min(rate, tcb->m_maxPacingRate);

    if (rate > tcb->m_pacingRate)
    {
        tcb->m_pacingRate = rate;
    }
    else
    {
        tcb->m_pacingRate = rate;
    }
    NS_LOG_DEBUG("Pacing rate: " << tcb->m_pacingRate);
}

void
TcpBbrV2::SetSendQuantum(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    m_sendQuantum = 1 * tcb->m_segmentSize;
}

void
TcpBbrV2::ModulateCwndForProbeRTT(Ptr<TcpSocketState> tcb) const
{
    NS_LOG_FUNCTION(this << tcb);
    if (m_state == BBRV2_PROBE_RTT)
    {
        // Linux BBRv2/v3 only halves the BDP in ProbeRTT; BBRv1 uses 4 packets.
        const uint32_t target =
            m_linuxLoss ? std::max(InFlight(tcb, 0.5), m_minPipeCwnd) : m_minPipeCwnd;
        tcb->m_cWnd = std::min(tcb->m_cWnd.Get(), target);
    }
}

bool
TcpBbrV2::ModulateCwndForRecovery(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    if (rs.m_bytesLoss > 0)
    {
        tcb->m_cWnd =
            std::max(static_cast<int>(tcb->m_cWnd.Get()) - static_cast<int>(rs.m_bytesLoss),
                     static_cast<int>(m_minPipeCwnd));
    }

    if (m_packetConservation)
    {
        tcb->m_cWnd = std::max(tcb->m_cWnd.Get(), tcb->m_bytesInFlight.Get() + rs.m_ackedSacked);
        return true;
    }
    return false;
}

void
TcpBbrV2::SetCwnd(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);

    if (!rs.m_ackedSacked)
    {
        ModulateCwndForProbeRTT(tcb);
        return;
    }

    if (tcb->m_congState == TcpSocketState::CA_RECOVERY)
    {
        if (ModulateCwndForRecovery(tcb, rs))
        {
            ModulateCwndForProbeRTT(tcb);
            return;
        }
    }

    m_targetCWnd = TargetCwnd(tcb, m_cWndGain);

    if (!m_linuxLoss && InLossRecovery() && m_inflightLo > 0)
    {
        // Drain back to the learned bound instead of growing.
        uint32_t bound = std::max(m_inflightLo, m_minPipeCwnd);
        if (tcb->m_cWnd > bound)
        {
            tcb->m_cWnd = std::max<uint32_t>(tcb->m_cWnd.Get() - rs.m_ackedSacked * 2, bound);
        }
    }
    else if (m_isPipeFilled)
    {
        tcb->m_cWnd = std::min<uint32_t>(tcb->m_cWnd.Get() + rs.m_ackedSacked, m_targetCWnd);
    }
    else if (tcb->m_cWnd < m_targetCWnd ||
             m_delivered < static_cast<uint64_t>(tcb->m_initialCWnd) * tcb->m_segmentSize)
    {
        tcb->m_cWnd = tcb->m_cWnd.Get() + rs.m_ackedSacked;
    }

    if (m_linuxLoss)
    {
        tcb->m_cWnd = BoundCwndForModel(tcb->m_cWnd.Get());
    }
    tcb->m_cWnd = std::max(tcb->m_cWnd.Get(), m_minPipeCwnd);
    NS_LOG_DEBUG("Congestion window: " << tcb->m_cWnd << " target: " << m_targetCWnd
                                       << " inflight_hi: " << m_inflightHi
                                       << " inflight_lo: " << m_inflightLo);

    ModulateCwndForProbeRTT(tcb);
}

void
TcpBbrV2::UpdateControlParameters(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);

    double gain = m_pacingGain;
    if (m_linuxLoss)
    {
        SetPacingRate(tcb, gain);
        if (m_bwLoLinux.GetBitRate() > 0)
        {
            DataRate lo(static_cast<uint64_t>(gain * m_bwLoLinux.GetBitRate() * 0.99));
            tcb->m_pacingRate = std::min({tcb->m_pacingRate.Get(), lo, tcb->m_maxPacingRate});
        }
        SetSendQuantum(tcb);
        SetCwnd(tcb, rs);
        return;
    }
    if (InLossRecovery() && m_inflightLo > 0 && m_minRtt != Time::Max())
    {
        // Do not pace faster than the drain bound.
        double loRate =
            (8.0 * std::max(m_inflightLo, m_minPipeCwnd)) / m_minRtt.Get().GetSeconds();
        if (m_linuxLoss && m_bwLoLinux.GetBitRate() > 0)
        {
            loRate = std::min(loRate, 1.0 * m_bwLoLinux.GetBitRate());
        }
        else if (!m_linuxLoss && m_bbrVersion == 3 && m_bwLoFilter.GetBest().GetBitRate() > 0)
        {
            // BBRv3 also bounds the recovery pacing by the minimum observed
            // bandwidth, so a stale inflight_lo cannot keep the rate too high.
            loRate = std::min(loRate, 1.0 * m_bwLoFilter.GetBest().GetBitRate());
        }
        DataRate rate(loRate);
        if (rate < DataRate(gain * m_maxBwFilter.GetBest().GetBitRate()))
        {
            tcb->m_pacingRate = std::min(rate, tcb->m_maxPacingRate);
            SetSendQuantum(tcb);
            SetCwnd(tcb, rs);
            return;
        }
    }

    SetPacingRate(tcb, gain);
    SetSendQuantum(tcb);
    SetCwnd(tcb, rs);
}

void
TcpBbrV2::CongControl(Ptr<TcpSocketState> tcb,
                      const TcpRateOps::TcpRateConnection& rc,
                      const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);
    m_delivered = rc.m_delivered;
    UpdateModelAndState(tcb, rs);
    UpdateControlParameters(tcb, rs);
}

void
TcpBbrV2::CongestionStateSet(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCongState_t newState)
{
    NS_LOG_FUNCTION(this << tcb << newState);
    if (newState == TcpSocketState::CA_OPEN && !m_isInitialized)
    {
        m_minRtt = tcb->m_srtt.Get() != Time::Max() ? tcb->m_srtt.Get() : Time::Max();
        m_minRttStamp = Simulator::Now();
        m_priorCwnd = tcb->m_cWnd;
        tcb->m_ssThresh = tcb->m_initialSsThresh;
        m_targetCWnd = tcb->m_cWnd;
        m_minPipeCwnd = 4 * tcb->m_segmentSize;
        m_sendQuantum = tcb->m_segmentSize;
        m_inflightHi = 0;
        m_inflightLo = 0;
        m_inflightLatest = 0;
        m_inflightHiMax = 0;
        m_lossRecovery = false;
        m_roundsSinceLoss = 0;
        m_minRttExpired = false;

        InitRoundCounting();
        InitFullPipe();
        EnterStartup();
        InitPacingRate(tcb);
        m_isInitialized = true;
    }
    else if (newState == TcpSocketState::CA_LOSS)
    {
        SaveCwnd(tcb);
        m_roundStart = true;
        if (m_linuxLoss && tcb->m_congState != TcpSocketState::CA_LOSS)
        {
            // Linux treats an RTO like the end of a round with loss: the
            // lower bounds adapt to what this round delivered.
            if (!IsProbingBandwidth())
            {
                AdaptLowerBounds(tcb);
            }
            else if (m_state == BBRV2_PROBE_BW)
            {
                StartProbeDown();
            }
            m_bwLatest = DataRate(0);
            m_inflightLatestRound = 0;
            m_lossInRound = false;
        }
    }
    else if (newState == TcpSocketState::CA_OPEN && m_linuxLoss &&
             tcb->m_congState == TcpSocketState::CA_LOSS)
    {
        m_packetConservation = false;
        RestoreCwnd(tcb);
    }
    else if (newState == TcpSocketState::CA_RECOVERY)
    {
        SaveCwnd(tcb);
        tcb->m_cWnd =
            tcb->m_bytesInFlight.Get() + std::max(tcb->m_lastAckedSackedBytes, tcb->m_segmentSize);
        m_packetConservation = true;
    }
}

void
TcpBbrV2::CwndEvent(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCAEvent_t event)
{
    NS_LOG_FUNCTION(this << tcb << event);
    if (event == TcpSocketState::CA_EVENT_COMPLETE_CWR)
    {
        m_packetConservation = false;
        RestoreCwnd(tcb);
    }
    else if (event == TcpSocketState::CA_EVENT_TX_START &&
             m_rateOps->GetConnectionRate().m_appLimited)
    {
        m_idleRestart = true;
        if (m_state == BBRV2_PROBE_BW)
        {
            SetPacingRate(tcb, 1);
        }
        else if (m_state == BBRV2_PROBE_RTT)
        {
            if (m_probeRttRoundDone && Simulator::Now() > m_probeRttDoneStamp)
            {
                m_minRttStamp = Simulator::Now();
                RestoreCwnd(tcb);
                ExitProbeRTT();
            }
        }
    }
}

uint32_t
TcpBbrV2::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    NS_LOG_FUNCTION(this << tcb << bytesInFlight);
    SaveCwnd(tcb);
    return tcb->m_ssThresh;
}

uint32_t
TcpBbrV2::GetBbrState() const
{
    return static_cast<uint32_t>(m_state);
}

uint32_t
TcpBbrV2::GetProbeBwPhase() const
{
    return static_cast<uint32_t>(m_phase);
}

double
TcpBbrV2::GetPacingGain() const
{
    return m_pacingGain;
}

double
TcpBbrV2::GetCwndGain() const
{
    return m_cWndGain;
}

uint32_t
TcpBbrV2::GetInflightHi() const
{
    return m_inflightHi;
}

uint32_t
TcpBbrV2::GetInflightLo() const
{
    return m_inflightLo;
}

std::string
TcpBbrV2::GetName() const
{
    return "TcpBbrV2";
}

bool
TcpBbrV2::HasCongControl() const
{
    return true;
}

Ptr<TcpCongestionOps>
TcpBbrV2::Fork()
{
    return CopyObject<TcpBbrV2>(this);
}

} // namespace ns3
