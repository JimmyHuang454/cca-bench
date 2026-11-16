/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copa congestion control, see tcp-copa.h for details.
 */

#include "tcp-copa.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpCopa");
NS_OBJECT_ENSURE_REGISTERED(TcpCopa);

/** Minimum congestion window, in segments. */
static const uint32_t COPA_MIN_CWND_SEGMENTS = 4;

/** Upper bound of the jitter floor [s]. */
static const double COPA_MAX_JITTER = 0.02;

/** Largest |ln(dq / dq*)| taken into one delta update (a factor of 4). */
static const double COPA_MAX_STEP = 2 * std::log(2.0);

TypeId
TcpCopa::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpCopa")
            .SetParent<TcpCongestionOps>()
            .AddConstructor<TcpCopa>()
            .SetGroupName("Internet")
            .AddAttribute("LatencyFactor",
                          "Delta of the default mode, controlling the latency sensitivity. "
                          "Lower values trade delay for throughput, higher values trade "
                          "throughput for delay.",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&TcpCopa::m_latencyFactor),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("MinRttWindowLength",
                          "Length of the windowed minimum RTT filter used to estimate the "
                          "propagation delay.",
                          TimeValue(Seconds(10)),
                          MakeTimeAccessor(&TcpCopa::m_minRttWindowLength),
                          MakeTimeChecker())
            .AddAttribute("ModeSwitching",
                          "Switch to the competitive mode (AIMD on 1/delta) when a "
                          "buffer-filling flow is detected (Section 2.2 of the paper).",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpCopa::m_modeSwitching),
                          MakeBooleanChecker())
            .AddAttribute("GenericCcVelocity",
                          "Use the velocity rule of the authors' implementation (genericCC): "
                          "the direction of a round is the majority of the per-ACK decisions "
                          "and the velocity doubles from the second round in the same "
                          "direction. False follows the paper (compare cwnd once per window, "
                          "double only after three rounds in the same direction).",
                          BooleanValue(false),
                          MakeBooleanAccessor(&TcpCopa::m_genericCcVelocity),
                          MakeBooleanChecker())
            .AddAttribute("AdaptiveDelta",
                          "Choose delta online: estimate the usable capacity, the jitter floor "
                          "and the burstiness of the wireless model and use the delta that "
                          "maximizes power (throughput / mean RTT). LatencyFactor is the "
                          "delta used until the estimates are ready.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&TcpCopa::m_adaptive),
                          MakeBooleanChecker())
            .AddAttribute("AdaptiveDeltaMin",
                          "Lower bound of the adaptive delta.",
                          DoubleValue(0.001),
                          MakeDoubleAccessor(&TcpCopa::m_adaptiveMin),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("AdaptiveDeltaMax",
                          "Upper bound of the adaptive delta.",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&TcpCopa::m_adaptiveMax),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("AdaptiveHalfRounds",
                          "Round trips per half-period (one delta, one measurement).",
                          UintegerValue(8),
                          MakeUintegerAccessor(&TcpCopa::m_halfRounds),
                          MakeUintegerChecker<uint32_t>(2))
            .AddAttribute("AdaptiveSettleRounds",
                          "Round trips skipped at the start of a half-period, while Copa "
                          "settles at the new delta.",
                          UintegerValue(3),
                          MakeUintegerAccessor(&TcpCopa::m_settleRounds),
                          MakeUintegerChecker<uint32_t>(0))
            .AddAttribute("AdaptiveProbeEvery",
                          "Half-periods per probing cycle; two of them run at AdaptiveDeltaMax "
                          "to expose the jitter floor.",
                          UintegerValue(120),
                          MakeUintegerAccessor(&TcpCopa::m_probeEvery),
                          MakeUintegerChecker<uint32_t>(8))
            .AddAttribute("AdaptiveWeight",
                          "Weight of a new half-period in the estimate of c / mu.",
                          DoubleValue(0.3),
                          MakeDoubleAccessor(&TcpCopa::m_weight),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("AdaptiveGain",
                          "Exponent g of the update delta <- delta (dq / dq*)^g, once per "
                          "half-period.",
                          DoubleValue(0.25),
                          MakeDoubleAccessor(&TcpCopa::m_gain),
                          MakeDoubleChecker<double>(0.0, 2.0))
            .AddAttribute("AdaptiveGainFar",
                          "Exponent of the delta update when dq differs from dq* by more than "
                          "the factor AdaptiveFarRatio.",
                          DoubleValue(0.25),
                          MakeDoubleAccessor(&TcpCopa::m_gainFar),
                          MakeDoubleChecker<double>(0.0, 2.0))
            .AddAttribute("AdaptiveFarRatio",
                          "dq / dq* (or its inverse) above which AdaptiveGainFar is used.",
                          DoubleValue(1.5),
                          MakeDoubleAccessor(&TcpCopa::m_farRatio),
                          MakeDoubleChecker<double>(1.0))
            .AddAttribute("AdaptiveProbePeriod",
                          "Period of the low-load probes aligned on absolute time (multiples "
                          "of this period), so that all flows of a host probe together and the "
                          "queue empties. Zero uses AdaptiveProbeEvery half-periods of each "
                          "flow instead. With a period, only the first probe after the slow "
                          "start is round-based.",
                          TimeValue(Seconds(20)),
                          MakeTimeAccessor(&TcpCopa::m_probePeriod),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveProbeDuration",
                          "Length of a time-aligned probe; its second half is measured.",
                          TimeValue(Seconds(2)),
                          MakeTimeAccessor(&TcpCopa::m_probeDuration),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveFairIncrease",
                          "When positive, 1/delta is raised additively by this many packets "
                          "times log(dq*/dq) when dq < dq*, and lowered multiplicatively "
                          "otherwise (AIMD on 1/delta), so that the deltas of the flows "
                          "sharing a bottleneck converge to the same value. Zero updates "
                          "1/delta multiplicatively in both directions, which keeps the "
                          "initial ratios between flows.",
                          DoubleValue(10.0),
                          MakeDoubleAccessor(&TcpCopa::m_fairIncrease),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("AdaptiveFairIncreaseFar",
                          "High-BDP far-below-target additive slope of 1/delta when dq < dq*/farRatio.",
                          DoubleValue(30.0),
                          MakeDoubleAccessor(&TcpCopa::m_fairIncreaseFar),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("AdaptiveIdleMargin",
                          "A packet did not queue when its RTT - RTTmin is at most the jitter "
                          "floor plus this margin.",
                          TimeValue(MilliSeconds(1)),
                          MakeTimeAccessor(&TcpCopa::m_idleMargin),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveProbes",
                          "With aligned probes, probe only until AdaptiveStableProbes probes "
                          "agree on the jitter floor, then in the fallback slots and after a "
                          "notable RTTmin change; the other slots drain. False probes in every "
                          "slot (the original schedule).",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpCopa::m_adaptiveProbes),
                          MakeBooleanChecker())
            .AddAttribute("AdaptiveStableProbes",
                          "Consecutive aligned probes that must agree on the jitter floor.",
                          UintegerValue(3),
                          MakeUintegerAccessor(&TcpCopa::m_stableProbes),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("AdaptiveStableTolerance",
                          "Two probes agree within max(this, AdaptiveStableRelTolerance * j).",
                          TimeValue(MicroSeconds(500)),
                          MakeTimeAccessor(&TcpCopa::m_stableTolAbs),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveStableRelTolerance",
                          "Relative part of the agreement tolerance.",
                          DoubleValue(0.1),
                          MakeDoubleAccessor(&TcpCopa::m_stableTolRel),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("AdaptiveReprobeRttChange",
                          "An RTTmin change by more than max(this, AdaptiveReprobeRttRel * "
                          "RTTmin) since the last probe starts a new estimate of j.",
                          TimeValue(MilliSeconds(5)),
                          MakeTimeAccessor(&TcpCopa::m_reprobeRttAbs),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveReprobeRttRel",
                          "Relative part of the RTTmin change that restarts j.",
                          DoubleValue(0.2),
                          MakeDoubleAccessor(&TcpCopa::m_reprobeRttRel),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("AdaptiveFallbackPeriod",
                          "Fallback probe period once j is settled; it doubles while the "
                          "probes agree, up to AdaptiveFallbackMax.",
                          TimeValue(Seconds(120)),
                          MakeTimeAccessor(&TcpCopa::m_fallbackPeriod),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveFallbackMax",
                          "Longest fallback period. Slots that are multiples of it always "
                          "probe, which also aligns flows of different groups.",
                          TimeValue(Seconds(600)),
                          MakeTimeAccessor(&TcpCopa::m_fallbackMax),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveRecovery",
                          "After an aligned probe, cwnd is restored to this fraction of its "
                          "value when the probe began (0 disables it).",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&TcpCopa::m_recovery),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("AdaptiveDrain",
                          "Enable short drains in non-probing aligned slots so RTTmin stays near "
                          "propagation delay under multi-flow queueing.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpCopa::m_drainEnabled),
                          MakeBooleanChecker())
            .AddAttribute("AdaptiveDrainMin",
                          "Shortest drain; zero disables the drains.",
                          TimeValue(MilliSeconds(50)),
                          MakeTimeAccessor(&TcpCopa::m_drainMin),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveDrainWindow",
                          "cwnd during a drain or gentle probe, as a fraction of the estimated BDP.",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&TcpCopa::m_drainWindowRatio),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("AdaptiveDrainSkip",
                          "No drain when a packet found the bottleneck idle within this time.",
                          TimeValue(Seconds(10)),
                          MakeTimeAccessor(&TcpCopa::m_drainSkip),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveJitterFloorMs",
                          "Fixed jitter floor j [ms]; negative enables online low-load probing.",
                          DoubleValue(-1.0),
                          MakeDoubleAccessor(&TcpCopa::m_jitterFloorMs),
                          MakeDoubleChecker<double>())
            .AddAttribute("AdaptiveGentleProbe",
                          "Hold AdaptiveDrainWindow * BDP during aligned jitter-floor probes "
                          "instead of jumping to AdaptiveDeltaMax.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpCopa::m_gentleProbe),
                          MakeBooleanChecker())
            .AddAttribute("AdaptiveProbeRtts",
                          "Probe length in smoothed RTTs, within [AdaptiveProbeDurationMin, "
                          "AdaptiveProbeDuration]; the longest one in the group is used. "
                          "Zero uses AdaptiveProbeDuration.",
                          UintegerValue(16),
                          MakeUintegerAccessor(&TcpCopa::m_probeRtts),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("AdaptiveProbeDurationMin",
                          "Shortest probe.",
                          TimeValue(MilliSeconds(300)),
                          MakeTimeAccessor(&TcpCopa::m_probeDurationMin),
                          MakeTimeChecker())
            .AddAttribute("AdaptiveBoardGroup",
                          "Flows with the same group (e.g. those of one host) share probe "
                          "requests: a flow that needs a probe registers it for the next slot "
                          "and all flows of the group probe in it.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&TcpCopa::m_boardGroup),
                          MakeUintegerChecker<uint32_t>())
            .AddTraceSource("AdaptiveEstimate",
                            "Estimates of the adaptive delta, once per round trip.",
                            MakeTraceSourceAccessor(&TcpCopa::m_adaptiveTrace),
                            "ns3::TcpCopa::AdaptiveEstimateTracedCallback");
    return tid;
}

TcpCopa::TcpCopa()
    : TcpCongestionOps()
{
    NS_LOG_FUNCTION(this);
    Reset();
}

TcpCopa::TcpCopa(const TcpCopa& sock)
    : TcpCongestionOps(sock),
      m_latencyFactor(sock.m_latencyFactor),
      m_minRttWindowLength(sock.m_minRttWindowLength),
      m_minCwnd(sock.m_minCwnd),
      m_modeSwitching(sock.m_modeSwitching),
      m_genericCcVelocity(sock.m_genericCcVelocity),
      m_adaptive(sock.m_adaptive),
      m_adaptiveMin(sock.m_adaptiveMin),
      m_adaptiveMax(sock.m_adaptiveMax),
      m_halfRounds(sock.m_halfRounds),
      m_settleRounds(sock.m_settleRounds),
      m_probeEvery(sock.m_probeEvery),
      m_weight(sock.m_weight),
      m_gain(sock.m_gain),
      m_gainFar(sock.m_gainFar),
      m_farRatio(sock.m_farRatio),
      m_fairIncrease(sock.m_fairIncrease),
      m_fairIncreaseFar(sock.m_fairIncreaseFar),
      m_idleMargin(sock.m_idleMargin),
      m_probePeriod(sock.m_probePeriod),
      m_probeDuration(sock.m_probeDuration),
      m_adaptiveProbes(sock.m_adaptiveProbes),
      m_stableProbes(sock.m_stableProbes),
      m_stableTolAbs(sock.m_stableTolAbs),
      m_stableTolRel(sock.m_stableTolRel),
      m_reprobeRttAbs(sock.m_reprobeRttAbs),
      m_reprobeRttRel(sock.m_reprobeRttRel),
      m_fallbackPeriod(sock.m_fallbackPeriod),
      m_fallbackMax(sock.m_fallbackMax),
      m_recovery(sock.m_recovery),
      m_drainEnabled(sock.m_drainEnabled),
      m_drainMin(sock.m_drainMin),
      m_drainWindowRatio(sock.m_drainWindowRatio),
      m_drainSkip(sock.m_drainSkip),
      m_jitterFloorMs(sock.m_jitterFloorMs),
      m_gentleProbe(sock.m_gentleProbe),
      m_probeRtts(sock.m_probeRtts),
      m_probeDurationMin(sock.m_probeDurationMin),
      m_boardGroup(sock.m_boardGroup)
{
    NS_LOG_FUNCTION(this);
    Reset();
}

TcpCopa::~TcpCopa()
{
    NS_LOG_FUNCTION(this);
    m_drainTimer.Cancel();
    m_lastTcb = nullptr;
}

void
TcpCopa::Reset()
{
    NS_LOG_FUNCTION(this);
    m_drainTimer.Cancel();
    m_lastTcb = nullptr;
    m_minRttFilter = RttFilter_t(std::max<uint64_t>(m_minRttWindowLength.GetMicroSeconds(), 1), 0, 0);
    m_standingRttFilter = RttFilter_t(1, 0, 0);
    m_maxRttFilter = RttMaxFilter_t(1, 0, 0);
    m_rttStandingUs = 0;
    m_delta = m_latencyFactor;
    m_competitive = false;
    m_lastNearlyEmpty = Simulator::Now();
    m_lastDeltaDecrease = Seconds(0);
    m_velocity = 1;
    m_direction = COPA_NONE;
    m_numTimesDirectionSame = 0;
    m_lastRecordedCwnd = 0;
    m_lastCwndRecordTime = Seconds(0);
    m_ackVotes = 0;
    m_prevVoteDir = 1;
    m_slowStart = true;
    m_lastSrtt = Seconds(0);
    m_jitter = m_jitterFloorMs >= 0.0 ? std::min(m_jitterFloorMs * 1e-3, COPA_MAX_JITTER) : -1.0;
    m_ratio = 0;
    m_rho = 0;
    m_baseRtt = 0;
    m_dqTarget = 0;
    m_deltaRun = 0;
    m_lastAckTime = Seconds(0);
    m_roundDqSum = 0;
    m_roundAcks = 0;
    m_roundIdle = 0;
    m_roundMaxGap = 0;
    m_roundMinRttUs = 0;
    m_roundStart = Seconds(0);
    m_rounds = 0;
    m_segDqSum = 0;
    m_segAcks = 0;
    m_segIdle = 0;
    m_segMaxGap = 0;
    m_segMinRttUs = 0;
    m_segProbe = false;
    m_floorShiftStreak = 0;
    m_handoverDrainPending = false;
    m_firstProbeDone = m_jitterFloorMs >= 0.0;
    m_alignedProbeSeen = m_jitterFloorMs >= 0.0;
    m_lastProbeDq = 0;
    m_probeDqSum = 0;
    m_probeAcks = 0;
    m_decidedSlot = UINT64_MAX;
    m_probingSlot = UINT64_MAX;
    m_slotDuration = 0;
    m_fallbackLevel = 0;
    m_agreeing = 0;
    m_stable = m_jitterFloorMs >= 0.0;
    m_resetNext = false;
    m_rttMinAtProbe = -1;
    m_probeHi = 0;
    m_settleHalves = 0;

    m_bwPrevAck = Seconds(-1);
    m_bwWindowStart = Seconds(-1);
    m_bwBucketStart = Seconds(-1);
    m_bwBucketDelivered = 0;
    m_bwCurMax = 0;
    m_bwPrevMax = 0;
    m_bwLastUpdate = Seconds(-1);
    m_bwPrevUpdate = Seconds(-1);

    m_drainPhase = PHASE_NORMAL;
    m_drainKind = DRAIN_REFRESH_RTT_MIN;
    m_drainHasSlot = false;
    m_drainSlot = 0;
    m_evaluatedSlot = UINT64_MAX;
    m_drainStart = Seconds(0);
    m_drainMaxEnd = Seconds(0);
    m_drainHoldFull = false;
    m_drainGain = 0.85;
    m_drainRate = 0;
    m_drainSoftCwnd = 0;
    m_savedCwnd = 0;
    m_savedRate = 0;
    m_savedRttMinUs = 0;
    m_trustedAtStartUs = 0;
    m_drainMinRttUs = 0;
    m_drainRoundStart = Seconds(0);
    m_drainRoundMinUs = 0;
    m_drainPrevRoundMinUs = 0;
    m_drainFlatRounds = 0;
    m_drainIdleSinceSet = false;
    m_drainIdleSince = Seconds(0);
    m_drainProbeDqSum = 0;
    m_drainProbeAcks = 0;
    m_drainCongested = false;
    m_drainBdpEst = 0;
    m_drainBackoffSlots = 1;
    m_nextDrainSlot = 0;
    m_lastIdle = Seconds(-1);
    m_trustedMinUs = 0;
    m_paceCapUntil = Seconds(0);
    m_paceCap = 0;
    m_lastAppLimited = false;

    m_probeSaved = false;
    m_savedCongested = false;
}

bool
TcpCopa::ProbingJitter() const
{
    return m_jitterFloorMs < 0.0;
}

bool
TcpCopa::InGentleProbe() const
{
    return ProbingJitter() && m_probePeriod.IsStrictlyPositive() && m_gentleProbe && !m_slowStart &&
           (!m_firstProbeDone || InTimedProbe());
}

bool
TcpCopa::InJitterProbe() const
{
    if (!ProbingJitter())
    {
        return false;
    }
    // Two half-periods at low load in every AdaptiveProbeEvery, the first ones
    // right after the slow start.
    const uint32_t half = (m_rounds / m_halfRounds) % m_probeEvery;
    if (m_probePeriod.IsStrictlyPositive())
    {
        // Only the first probe, until the queue left by the slow start has drained.
        return !m_firstProbeDone;
    }
    return half <= 1;
}

TcpCopa::ProbeBoard&
TcpCopa::Board() const
{
    static std::map<uint32_t, ProbeBoard> boards;
    return boards[m_boardGroup];
}

uint64_t
TcpCopa::Slot() const
{
    return static_cast<uint64_t>(
        std::floor(Simulator::Now().GetSeconds() / m_probePeriod.GetSeconds()));
}

double
TcpCopa::Phase() const
{
    return std::fmod(Simulator::Now().GetSeconds(), m_probePeriod.GetSeconds());
}

Time
TcpCopa::WantedDuration(Time srtt) const
{
    if (m_probeRtts == 0)
    {
        return m_probeDuration;
    }
    return std::clamp(srtt * m_probeRtts, std::min(m_probeDurationMin, m_probeDuration), m_probeDuration);
}

uint64_t
TcpCopa::FallbackEvery(uint32_t level) const
{
    const double base = m_fallbackPeriod.GetSeconds();
    const double max = std::max(m_fallbackMax.GetSeconds(), base);
    const double seconds = std::min(base * std::pow(2.0, std::min(level, 60u)), max);
    return std::max<uint64_t>(1, std::llround(seconds / m_probePeriod.GetSeconds()));
}

uint32_t
TcpCopa::MaxFallbackLevel() const
{
    const double base = std::max(m_fallbackPeriod.GetSeconds(), 1e-9);
    const double max = std::max(m_fallbackMax.GetSeconds(), base);
    return static_cast<uint32_t>(std::max(0.0, std::ceil(std::log2(max / base))));
}

bool
TcpCopa::WantsProbe(uint64_t slot) const
{
    if (!ProbingJitter())
    {
        return false;
    }
    if (!m_adaptiveProbes || !m_firstProbeDone || !m_stable)
    {
        return true;
    }
    return slot % FallbackEvery(m_fallbackLevel) == 0;
}

bool
TcpCopa::RttMinChanged(double a, double b) const
{
    return std::abs(a - b) > std::max(m_reprobeRttAbs.GetSeconds(), m_reprobeRttRel * std::max(a, b));
}

void
TcpCopa::Unsettle()
{
    m_stable = false;
    m_agreeing = 0;
    m_resetNext = true;
    m_fallbackLevel = 0;
}

bool
TcpCopa::InTimedProbe() const
{
    if (!ProbingJitter() || !m_probePeriod.IsStrictlyPositive() || m_slowStart)
    {
        return false;
    }
    if (!m_adaptiveProbes)
    {
        return Phase() < m_probeDuration.GetSeconds();
    }
    return m_probingSlot == Slot() && Phase() < m_slotDuration;
}

uint64_t
TcpCopa::DrainSlot() const
{
    if (!m_adaptiveProbes || !m_drainEnabled || !m_drainMin.IsStrictlyPositive() ||
        !m_probePeriod.IsStrictlyPositive() || !m_firstProbeDone || m_slowStart)
    {
        return UINT64_MAX;
    }
    const uint64_t slot = Slot();
    if (m_decidedSlot != slot || Phase() >= m_slotDuration || m_probingSlot == slot)
    {
        return UINT64_MAX;
    }
    return slot;
}

void
TcpCopa::RecordAlignedProbe(double probeDq, uint64_t rttMinUs)
{
    const bool fresh = !m_alignedProbeSeen || m_resetNext;
    const double old = m_jitter;
    const double j = (!fresh && old >= 0) ? std::min(old, probeDq) : probeDq;
    if (m_adaptiveProbes)
    {
        auto tol = [this](double x) {
            return std::max(m_stableTolAbs.GetSeconds(), m_stableTolRel * x);
        };
        if (old >= 0 && !fresh && m_stable &&
            probeDq > std::max(1.5 * m_probeHi, m_probeHi + tol(m_probeHi)))
        {
            // A fallback probe well above every probe seen while settling: the floor rose
            Unsettle();
        }
        else if (old >= 0 && !fresh && old - j <= tol(old))
        {
            m_agreeing++;
        }
        else
        {
            m_agreeing = 0;
        }
        if (fresh)
        {
            m_resetNext = false;
            m_probeHi = probeDq;
        }
        else if (!m_resetNext)
        {
            m_probeHi = std::max(m_probeHi, probeDq);
        }
        const bool wasStable = m_stable;
        m_stable = !m_resetNext && m_agreeing + 1 >= std::max(1u, m_stableProbes);
        if (wasStable && m_stable)
        {
            // A fallback probe agreed: wait twice as long for the next one
            m_fallbackLevel = std::min(m_fallbackLevel + 1, MaxFallbackLevel());
        }
        m_rttMinAtProbe = rttMinUs * 1e-6;
    }
    m_jitter = std::min(j, COPA_MAX_JITTER);
    m_alignedProbeSeen = true;
}

void
TcpCopa::FinishGentleProbe(bool hasSlot,
                           uint64_t slot,
                           bool hasProbeDq,
                           double probeDq,
                           uint64_t rttMinUs)
{
    if (hasProbeDq)
    {
        if (!m_firstProbeDone)
        {
            m_firstProbeDone = true;
            m_settleHalves = 1;
            if (m_jitter < 0)
            {
                m_jitter = std::min(probeDq, COPA_MAX_JITTER);
            }
        }
        if (hasSlot)
        {
            RecordAlignedProbe(probeDq, rttMinUs);
        }
    }
    else if (!m_firstProbeDone)
    {
        m_firstProbeDone = true;
        m_settleHalves = 1;
    }
    if (hasSlot)
    {
        m_decidedSlot = slot;
        if (m_probingSlot == slot)
        {
            m_probingSlot = UINT64_MAX;
        }
    }
    else if (m_probePeriod.IsStrictlyPositive())
    {
        // Initial calibration just finished: avoid immediately re-probing/draining the current
        // slot if still within slot 0's probe window.
        const uint64_t curSlot = Slot();
        m_evaluatedSlot = curSlot;
        m_decidedSlot = curSlot;
        m_probingSlot = UINT64_MAX;
    }
    m_probeDqSum = 0;
    m_probeAcks = 0;
}

void
TcpCopa::UpdateAdaptiveDelta(Ptr<TcpSocketState> tcb,
                             uint32_t ackedBytes,
                             Time lrtt,
                             uint64_t rttMinUs,
                             uint64_t dqUs,
                             Time srtt,
                             bool excluded)
{
    NS_LOG_FUNCTION(this << tcb << ackedBytes << lrtt << rttMinUs << dqUs << srtt << excluded);
    const Time now = Simulator::Now();
    if (m_roundStart.IsZero())
    {
        m_roundStart = now;
    }
    else
    {
        m_roundMaxGap = std::max(m_roundMaxGap, (now - m_lastAckTime).GetSeconds());
    }
    m_lastAckTime = now;
    m_segProbe = m_segProbe || excluded;

    const bool aligned = m_probePeriod.IsStrictlyPositive();
    const bool gentle = aligned && ProbingJitter() && m_gentleProbe;
    if (m_adaptiveProbes && aligned)
    {
        // A path change shows as a jump of RTTmin: the floor measured before may not hold
        if (ProbingJitter() && m_rttMinAtProbe >= 0 && m_stable &&
            RttMinChanged(m_rttMinAtProbe, rttMinUs * 1e-6))
        {
            Unsettle();
        }
        if (!m_slowStart)
        {
            ProbeBoard& board = Board();
            const uint64_t slot = Slot();
            const Time own = WantedDuration(srtt);
            auto publish = [&board](uint64_t s, Time length, bool request) {
                const uint32_t i = s % 4;
                if (request)
                {
                    board.requests[i] = std::max(board.requests[i], s + 1);
                }
                const uint64_t us = std::max<int64_t>(1, length.GetMicroSeconds());
                if (board.lengthSlot[i] != s + 1)
                {
                    board.lengthSlot[i] = std::max(board.lengthSlot[i], s + 1);
                    board.lengthUs[i] = us;
                }
                else
                {
                    board.lengthUs[i] = std::max(board.lengthUs[i], us);
                }
            };
            // Register ahead, once this slot's probe window is over, for the flows deciding
            // at the next slot boundary
            if (ProbingJitter() && m_firstProbeDone && Phase() >= m_probeDuration.GetSeconds())
            {
                publish(slot + 1, own, WantsProbe(slot + 1));
            }
            // Decide once per slot, at its first ACK, so the probe keeps its length
            if (m_decidedSlot != slot && Phase() < m_probeDuration.GetSeconds())
            {
                m_decidedSlot = slot;
                if (ProbingJitter())
                {
                    publish(slot, own, WantsProbe(slot));
                    const uint32_t i = slot % 4;
                    Time length = own;
                    if (board.lengthSlot[i] == slot + 1)
                    {
                        length = std::max(length, MicroSeconds(board.lengthUs[i]));
                    }
                    m_slotDuration = length.GetSeconds();
                    const bool forced = slot % FallbackEvery(UINT32_MAX >> 1) == 0;
                    if (board.requests[i] == slot + 1 || forced)
                    {
                        m_probingSlot = slot;
                    }
                }
                else
                {
                    m_slotDuration = own.GetSeconds();
                }
            }
        }
    }

    if (!gentle)
    {
        if (InTimedProbe())
        {
            m_segProbe = true;
            const double duration =
                m_adaptiveProbes ? m_slotDuration : m_probeDuration.GetSeconds();
            // A probe that overlaps the first one still holds the slow-start queue
            if (m_firstProbeDone && Phase() >= 0.5 * duration)
            {
                m_probeDqSum += dqUs * 1e-6;
                m_probeAcks++;
            }
        }
        else if (m_probeAcks > 0)
        {
            // End of a legacy time-aligned probe: mean dq of its second half is the jitter floor.
            const double probeDq = m_probeDqSum / m_probeAcks;
            RecordAlignedProbe(probeDq, rttMinUs);
            NS_LOG_INFO("aligned probe dq=" << probeDq << " acks=" << m_probeAcks << " j="
                                            << m_jitter << " stable=" << m_stable);
            m_probeDqSum = 0;
            m_probeAcks = 0;
        }
    }
    m_roundDqSum += dqUs * 1e-6;
    m_roundAcks++;
    const uint64_t lrttUs = static_cast<uint64_t>(std::max<int64_t>(lrtt.GetMicroSeconds(), 0));
    m_roundMinRttUs = (m_roundMinRttUs == 0) ? lrttUs : std::min(m_roundMinRttUs, lrttUs);
    // The packet found the bottleneck idle: its RTT is the propagation delay
    // plus the jitter floor.
    const double pktDelay = lrtt.GetSeconds() - rttMinUs * 1e-6;
    if (m_jitter >= 0 && pktDelay <= m_jitter + m_idleMargin.GetSeconds())
    {
        m_roundIdle++;
    }
    const double roundTime = (now - m_roundStart).GetSeconds();
    if (roundTime < srtt.GetSeconds() || m_roundAcks < 2)
    {
        return;
    }
    if (!m_slowStart)
    {
        // Per round, rate and dq follow Copa's velocity dynamics; a point of
        // the equilibrium is the average over a half-period at one delta,
        // once Copa has settled. The first half-period of a low-load probe
        // only lets Copa settle.
        const bool probe = InJitterProbe();
        const bool settling = probe && (m_rounds / m_halfRounds) % m_probeEvery == 0;
        if (!settling && m_rounds % m_halfRounds >= m_settleRounds)
        {
            m_segDqSum += m_roundDqSum;
            m_segAcks += m_roundAcks;
            m_segIdle += m_roundIdle;
            m_segMaxGap = std::max(m_segMaxGap, m_roundMaxGap);
            if (m_roundMinRttUs > 0)
            {
                m_segMinRttUs = (m_segMinRttUs == 0) ? m_roundMinRttUs : std::min(m_segMinRttUs, m_roundMinRttUs);
            }
        }
        m_rounds++;
        if (m_rounds % m_halfRounds == 0 && !settling)
        {
            // The half-period right after the first probe: Copa is still leaving the probe's
            // equilibrium
            const bool settlingAfterProbe = m_settleHalves > 0 && !probe;
            if (settlingAfterProbe)
            {
                m_settleHalves--;
            }
            // A half-period with an outage (handover, radio link failure), in the competitive
            // mode, during a drain or a recovery is not an equilibrium point.
            const bool valid = m_segAcks > 0 && !settlingAfterProbe && !m_competitive &&
                               !m_segProbe && m_segMaxGap < std::max(0.05, srtt.GetSeconds());
            const double segDq = valid ? m_segDqSum / m_segAcks : 0;
            if (!gentle && m_probePeriod.IsStrictlyPositive() && !m_firstProbeDone)
            {
                // The first probe lasts until dq stops falling (at most eight
                // half-periods): earlier samples still hold the slow-start queue.
                if ((valid && m_lastProbeDq > 0 && segDq >= 0.9 * m_lastProbeDq) ||
                    m_rounds / m_halfRounds >= 8)
                {
                    m_firstProbeDone = true;
                    m_settleHalves = 1;
                }
                if (valid)
                {
                    m_lastProbeDq = segDq;
                }
            }
            if (valid && probe)
            {
                // Jitter floor: minimum over the low-load probes. Other flows
                // keep the queue busy during a probe, so a probe can only
                // overestimate it; the minimum cannot drift upwards.
                m_jitter = std::min(m_jitter < 0 ? segDq : std::min(m_jitter, segDq), COPA_MAX_JITTER);
            }
            else if (valid && m_jitter >= 0)
            {
                const uint64_t shiftUs =
                    (m_segMinRttUs > rttMinUs) ? (m_segMinRttUs - rttMinUs) : 0;
                const double floorShift = shiftUs * 1e-6;
                const double shiftThresh = std::max(0.005, 0.15 * (rttMinUs * 1e-6));
                const bool shiftedBaseline =
                    (m_deltaRun > 0) &&
                    (floorShift > m_jitter + shiftThresh) &&
                    (segDq > 2.0 * std::max(m_dqTarget, 0.005)) &&
                    (segDq - floorShift < floorShift);
                if (shiftedBaseline)
                {
                    m_floorShiftStreak++;
                    if (m_floorShiftStreak >= 2 && m_drainEnabled)
                    {
                        m_handoverDrainPending = true;
                    }
                }
                else
                {
                    m_floorShiftStreak = 0;
                    // Total utilization from the idle fraction, then k = c / mu
                    // from dq = j + k rho / (1 - rho).
                    m_rho = std::clamp(1.0 - 1.0 * m_segIdle / m_segAcks, 0.05, 0.98);
                    const double queueing = std::max(segDq - m_jitter, 1e-5);
                    const double k = queueing * (1 - m_rho) / m_rho;
                    m_ratio = (m_ratio <= 0) ? k
                                             : std::exp((1 - m_weight) * std::log(m_ratio) +
                                                        m_weight * std::log(k));
                    m_baseRtt = rttMinUs * 1e-6 + m_jitter;
                    m_dqTarget = m_jitter + std::sqrt(m_ratio * m_baseRtt);
                    // delta <- 1 / (lambda dq*) = delta dq / dq* at equilibrium.
                    const double base = m_deltaRun > 0 ? m_deltaRun : m_delta;
                    // An empty queue would give log(0), and a half-period far off the target must
                    // not throw delta to its bound in one step. Large steps far from the target,
                    // small steps near it, where the measurement noise dominates.
                    const double err = std::clamp(std::log(std::max(segDq, 1e-5) / m_dqTarget),
                                                  -COPA_MAX_STEP,
                                                  COPA_MAX_STEP);
                    const double gain =
                        std::abs(err) > std::log(m_farRatio) ? m_gainFar : m_gain;
                    double next = base * std::exp(gain * err);
                    if (m_fairIncrease > 0 && err < 0)
                    {
                        // dq below the target: the same additive step of 1/delta for every flow
                        // near equilibrium, scaling the far-below-target slope on high-BDP links.
                        const double bwEst = EstimateBw(now, srtt);
                        const double mss = (tcb && tcb->m_segmentSize > 0) ? tcb->m_segmentSize : 1448.0;
                        const double bdpPkts = (bwEst > 0.0) ? (bwEst * (rttMinUs * 1e-6) / mss) : 0.0;
                        const double farFrac = std::clamp((bdpPkts - 150.0) / 500.0, 0.0, 1.0);
                        const double slope =
                            (-err > std::log(m_farRatio))
                                ? (m_fairIncrease + farFrac * std::max(0.0, m_fairIncreaseFar - m_fairIncrease))
                                : m_fairIncrease;
                        next = 1.0 / (1.0 / base - slope * err);
                    }
                    m_deltaRun = std::clamp(next, m_adaptiveMin, m_adaptiveMax);
                    NS_LOG_INFO("half dq=" << segDq << " rho=" << m_rho << " j=" << m_jitter
                                           << " k=" << m_ratio << " dq*=" << m_dqTarget
                                           << " delta=" << m_deltaRun);
                }
            }
            m_segDqSum = 0;
            m_segAcks = 0;
            m_segIdle = 0;
            m_segMaxGap = 0;
            m_segMinRttUs = 0;
            m_segProbe = false;
        }
    }
    m_adaptiveTrace(std::max(m_jitter, 0.0), m_ratio, m_rho, m_baseRtt, m_dqTarget, m_delta);
    m_roundStart = now;
    m_roundDqSum = 0;
    m_roundAcks = 0;
    m_roundIdle = 0;
    m_roundMaxGap = 0;
    m_roundMinRttUs = 0;
}

void
TcpCopa::UpdateBwEstimate(Time now, uint32_t ackedBytes, Time srtt, uint32_t cwnd, bool appLimited)
{
    if (m_drainPhase != PHASE_NORMAL)
    {
        m_bwWindowStart = Seconds(-1);
        m_bwBucketDelivered = 0;
        m_bwPrevAck = now;
        return;
    }
    const Time minWin = std::max(srtt, MilliSeconds(10));
    if (m_bwPrevAck >= Seconds(0) && (now - m_bwPrevAck) > minWin * 2)
    {
        m_bwWindowStart = now;
        m_bwBucketDelivered = 0;
    }
    if (m_bwWindowStart < Seconds(0))
    {
        m_bwWindowStart = now;
    }
    m_bwBucketDelivered += ackedBytes;
    m_bwPrevAck = now;

    const Time elapsed = now - m_bwWindowStart;
    if (elapsed < minWin || m_bwBucketDelivered == 0)
    {
        return;
    }
    if (m_bwBucketDelivered < cwnd / 2 && elapsed < minWin * 4)
    {
        return;
    }

    const double bw = static_cast<double>(m_bwBucketDelivered) / elapsed.GetSeconds();
    if (m_bwBucketStart < Seconds(0))
    {
        m_bwBucketStart = now;
    }
    if (!appLimited && (now - m_bwBucketStart) >= minWin * 2)
    {
        m_bwPrevMax = m_bwCurMax;
        m_bwPrevUpdate = m_bwLastUpdate;
        m_bwCurMax = 0.0;
        m_bwLastUpdate = Seconds(-1);
        m_bwBucketStart = now;
    }

    const double curEst = EstimateBw(now, srtt);
    if ((!appLimited || (curEst > 0.0 && bw > curEst)) && bw >= m_bwCurMax)
    {
        m_bwCurMax = bw;
        m_bwLastUpdate = now;
    }
    m_bwWindowStart = now;
    m_bwBucketDelivered = 0;
}

double
TcpCopa::EstimateBw(Time now, Time srtt) const
{
    const Time maxAge = std::max(8 * srtt, Seconds(1));
    double best = 0.0;
    if (m_bwLastUpdate >= Seconds(0) && (now - m_bwLastUpdate) <= maxAge)
    {
        best = std::max(best, m_bwCurMax);
    }
    if (m_bwPrevUpdate >= Seconds(0) && (now - m_bwPrevUpdate) <= maxAge)
    {
        best = std::max(best, m_bwPrevMax);
    }
    return best;
}

bool
TcpCopa::ShouldDrainSlot(Time now, uint64_t slot, uint64_t rttMinUs, bool boardRequested) const
{
    if (boardRequested)
    {
        return true;
    }
    if (m_trustedMinUs == 0)
    {
        return true;
    }
    if (slot < m_nextDrainSlot)
    {
        return false;
    }
    const uint64_t driftMargin = std::max<uint64_t>(1000, m_trustedMinUs / 50);
    if (rttMinUs > m_trustedMinUs + driftMargin)
    {
        return true;
    }
    const bool idleFresh = m_lastIdle >= Seconds(0) && (now - m_lastIdle) < m_drainSkip;
    return !idleFresh;
}

void
TcpCopa::ScheduleDrainTimer(Time until)
{
    m_drainTimer.Cancel();
    const Time now = Simulator::Now();
    if (until > now)
    {
        m_drainTimer = Simulator::Schedule(until - now, &TcpCopa::OnDrainTimer, this);
    }
}

void
TcpCopa::StartDrain(Ptr<TcpSocketState> tcb,
                    DrainKind kind,
                    bool hasSlot,
                    uint64_t slot,
                    Time now,
                    Time srtt,
                    uint64_t rttMinUs,
                    uint64_t rttStandingUs,
                    uint64_t dqUs,
                    Time maxDuration,
                    bool holdFull)
{
    m_lastTcb = tcb;
    m_drainPhase = PHASE_DRAINING;
    m_drainKind = kind;
    m_drainHasSlot = hasSlot;
    m_drainSlot = slot;
    m_drainStart = now;

    const Time srttSafe = std::max(srtt, MilliSeconds(1));
    const double bw = EstimateBw(now, srttSafe);
    const uint64_t baseRttUs =
        (m_trustedMinUs > 0) ? std::min(m_trustedMinUs, rttMinUs) : rttMinUs;
    const double baseRttSec = std::max(baseRttUs * 1e-6, 1e-6);
    const double standingSec = std::max(rttStandingUs * 1e-6, baseRttSec);
    const double cwndRate = static_cast<double>(tcb->m_cWnd.Get()) / standingSec;
    const double minRateFloor = static_cast<double>(m_minCwnd) / standingSec;

    const double gain = (kind == DRAIN_MEASURE_JITTER) ? 0.78 : (dqUs > 20000 ? 0.75 : 0.85);
    m_drainGain = gain;

    const double savedRate =
        (m_rttStandingUs > 0)
            ? 2.0 * static_cast<double>(tcb->m_cWnd.Get()) / (m_rttStandingUs * 1e-6)
            : 0.0;
    m_savedRate = savedRate;

    const double refBw =
        (bw > 0.0) ? std::max(std::min(bw, 1.5 * cwndRate), minRateFloor)
                   : std::max(cwndRate, minRateFloor);
    m_drainRate =
        std::max(gain * refBw, static_cast<double>(m_minCwnd) / std::max(srttSafe.GetSeconds(), 0.01));

    const double bdp = refBw * baseRttSec;
    m_drainBdpEst = static_cast<uint64_t>(std::max<double>(m_minCwnd, bdp));
    m_drainSoftCwnd = std::min<uint32_t>(
        tcb->m_cWnd.Get(),
        std::max<uint32_t>(m_minCwnd, static_cast<uint32_t>(0.90 * m_drainBdpEst)));

    m_savedCwnd = tcb->m_cWnd.Get();
    m_savedRttMinUs = rttMinUs;
    m_trustedAtStartUs = (m_trustedMinUs > 0) ? m_trustedMinUs : rttMinUs;
    m_drainMinRttUs = 0;
    m_drainRoundStart = now;
    m_drainRoundMinUs = 0;
    m_drainPrevRoundMinUs = 0;
    m_drainFlatRounds = 0;
    m_drainIdleSinceSet = false;
    m_drainIdleSince = Seconds(0);
    m_drainProbeDqSum = 0.0;
    m_drainProbeAcks = 0;
    m_drainCongested = false;
    m_drainHoldFull = holdFull;

    if (kind == DRAIN_MEASURE_JITTER)
    {
        m_drainMaxEnd = now + std::max(maxDuration, MilliSeconds(100));
    }
    else
    {
        const Time est = MicroSeconds(2 * dqUs) + 3 * srttSafe;
        const Time hardMax = std::clamp(std::max(maxDuration, est), MilliSeconds(50), Seconds(1));
        m_drainMaxEnd = now + hardMax;
    }

    ScheduleDrainTimer(m_drainMaxEnd);
    NS_LOG_INFO("drain start kind=" << kind << " gain=" << gain << " softCwnd=" << m_drainSoftCwnd
                                    << " rate=" << m_drainRate << " until=" << m_drainMaxEnd);
}

uint32_t
TcpCopa::CurrentDrainWindow(Time now, Time srtt) const
{
    const double rampSec = std::max(srtt.GetSeconds(), 1e-3);
    const double frac = std::clamp((now - m_drainStart).GetSeconds() / rampSec, 0.0, 1.0);
    const double w = static_cast<double>(m_savedCwnd) +
                     frac * (static_cast<double>(m_drainSoftCwnd) -
                             static_cast<double>(m_savedCwnd));
    return static_cast<uint32_t>(std::clamp<double>(w, m_minCwnd, static_cast<double>(1ull << 31)));
}

bool
TcpCopa::OnDrainAck(Time now,
                  Time lrtt,
                  uint64_t rttMinUs,
                  uint64_t rttStandingUs,
                  uint64_t dqUs,
                  Time srtt)
{
    if (m_drainPhase != PHASE_DRAINING)
    {
        return false;
    }
    const uint64_t sampleUs = lrtt.GetMicroSeconds();
    if (sampleUs > 0)
    {
        m_drainMinRttUs =
            (m_drainMinRttUs == 0) ? sampleUs : std::min(m_drainMinRttUs, sampleUs);
        m_drainRoundMinUs =
            (m_drainRoundMinUs == 0) ? sampleUs : std::min(m_drainRoundMinUs, sampleUs);
    }

    const Time srttSafe = std::max(srtt, MilliSeconds(10));
    if ((now - m_drainRoundStart) >= srttSafe && m_drainRoundMinUs > 0)
    {
        if (m_drainPrevRoundMinUs > 0)
        {
            const uint64_t tol = std::max<uint64_t>(1000, m_drainRoundMinUs / 50);
            if (m_drainPrevRoundMinUs <= m_drainRoundMinUs + tol)
            {
                m_drainFlatRounds++;
            }
            else
            {
                m_drainFlatRounds = 0;
            }
        }
        m_drainPrevRoundMinUs = m_drainRoundMinUs;
        m_drainRoundMinUs = 0;
        m_drainRoundStart = now;
    }

    if (now >= m_drainMaxEnd)
    {
        return true;
    }

    const Time minHold = std::max(srtt, MilliSeconds(20));
    const Time elapsed = now - m_drainStart;
    if (elapsed < minHold)
    {
        return false;
    }

    const uint64_t jitterUs = (m_jitter > 0.0) ? static_cast<uint64_t>(m_jitter * 1e6) : 0;
    const uint64_t baseTolUs = std::max<uint64_t>(1000, m_trustedAtStartUs / 20);
    const bool backToBaseline = (rttStandingUs <= m_trustedAtStartUs + jitterUs + baseTolUs);
    const bool plateau = (elapsed >= 2 * srtt) && (m_drainFlatRounds >= 2);

    if (m_drainKind == DRAIN_REFRESH_RTT_MIN)
    {
        return backToBaseline || plateau;
    }

    if (m_drainHoldFull)
    {
        if (now >= m_drainStart + (m_drainMaxEnd - m_drainStart) / 2)
        {
            m_drainProbeDqSum += dqUs * 1e-6;
            m_drainProbeAcks++;
        }
        return false;
    }

    if (!m_drainIdleSinceSet && (backToBaseline || plateau))
    {
        m_drainIdleSinceSet = true;
        m_drainIdleSince = now;
    }
    if (m_drainIdleSinceSet)
    {
        m_drainProbeDqSum += dqUs * 1e-6;
        m_drainProbeAcks++;
        const Time jWin = std::clamp(5 * srtt, MilliSeconds(100), MilliSeconds(600));
        if ((now - m_drainIdleSince) >= jWin)
        {
            return true;
        }
    }
    return false;
}

void
TcpCopa::FinishDrain(Ptr<TcpSocketState> tcb, uint64_t rttMinUs)
{
    m_drainTimer.Cancel();
    const Time now = Simulator::Now();
    m_lastNearlyEmpty = now;

    const bool hasProbeDq = (m_drainKind == DRAIN_MEASURE_JITTER && m_drainProbeAcks > 0);
    const double probeDq = hasProbeDq ? (m_drainProbeDqSum / m_drainProbeAcks) : 0.0;
    const bool plateauConverged =
        !m_drainCongested &&
        m_drainKind == DRAIN_REFRESH_RTT_MIN &&
        !m_drainHasSlot &&
        m_drainFlatRounds >= 2 &&
        m_drainMinRttUs > 0;
    const uint64_t shiftThreshUs = std::max<uint64_t>(5000, rttMinUs * 15 / 100);
    const bool baselineRose =
        plateauConverged &&
        m_drainMinRttUs > rttMinUs &&
        (m_drainMinRttUs - rttMinUs) > shiftThreshUs;
    uint64_t effectiveMin = (m_drainMinRttUs > 0) ? m_drainMinRttUs : rttMinUs;
    if (baselineRose)
    {
        m_minRttFilter =
            RttFilter_t(std::max<uint64_t>(m_minRttWindowLength.GetMicroSeconds(), 1), 0, 0);
        m_minRttFilter.Update(m_drainMinRttUs, now.GetMicroSeconds());
        m_trustedMinUs = m_drainMinRttUs;
        m_floorShiftStreak = 0;
        m_handoverDrainPending = false;
        effectiveMin = m_drainMinRttUs;
    }
    else if (effectiveMin > 0)
    {
        m_trustedMinUs = effectiveMin;
    }

    if (m_drainKind == DRAIN_MEASURE_JITTER)
    {
        FinishGentleProbe(m_drainHasSlot, m_drainSlot, hasProbeDq, probeDq, effectiveMin);
    }
    else
    {
        const uint64_t margin = std::max<uint64_t>(1000, m_savedRttMinUs / 50);
        if (m_drainMinRttUs > 0 && m_savedRttMinUs >= m_drainMinRttUs + margin)
        {
            m_drainBackoffSlots = 1;
        }
        else
        {
            m_drainBackoffSlots = std::min<uint32_t>(m_drainBackoffSlots * 2, 8);
        }
        if (m_drainHasSlot)
        {
            m_nextDrainSlot = m_drainSlot + m_drainBackoffSlots;
        }
    }

    const bool recover =
        !m_drainCongested && (m_drainKind == DRAIN_REFRESH_RTT_MIN || m_recovery > 0.0);
    if (recover && tcb)
    {
        const bool movedDown =
            (!baselineRose && m_drainMinRttUs > 0 && m_savedRttMinUs > m_drainMinRttUs &&
             RttMinChanged(m_savedRttMinUs * 1e-6, m_drainMinRttUs * 1e-6));
        uint64_t target = m_savedCwnd;
        if (baselineRose)
        {
            target = std::max<uint64_t>(m_drainBdpEst, m_savedCwnd);
        }
        else if (movedDown)
        {
            target = std::min<uint64_t>(m_drainBdpEst, m_savedCwnd);
        }
        tcb->m_cWnd = std::max<uint32_t>(
            tcb->m_cWnd.Get(),
            static_cast<uint32_t>(std::clamp<uint64_t>(target, m_minCwnd, 1ull << 31)));
        m_velocity = 1;
        m_direction = COPA_NONE;
        m_numTimesDirectionSame = 0;
        m_lastRecordedCwnd = 0;
        m_lastCwndRecordTime = Seconds(0);
        m_ackVotes = 0;
        m_prevVoteDir = 1;
    }

    const Time srttSafe = std::max(m_lastSrtt, MilliSeconds(1));
    if (recover && m_savedRate > 0.0)
    {
        m_drainPhase = PHASE_RECOVERING;
        m_paceCap = m_savedRate;
        m_paceCapUntil = now + srttSafe;
        ScheduleDrainTimer(m_paceCapUntil);
    }
    else
    {
        m_drainPhase = PHASE_NORMAL;
        m_paceCap = 0.0;
        m_paceCapUntil = Seconds(0);
    }
    if (tcb)
    {
        SetPacingRate(tcb);
    }
}

void
TcpCopa::PollDrainExpiry(Ptr<TcpSocketState> tcb, Time srtt, uint64_t rttMinUs)
{
    const Time now = Simulator::Now();
    if (m_drainPhase == PHASE_DRAINING && now >= m_drainMaxEnd)
    {
        const uint64_t fallbackMin = (rttMinUs > 0) ? rttMinUs : m_savedRttMinUs;
        FinishDrain(tcb, fallbackMin);
    }
    if (m_drainPhase == PHASE_RECOVERING && now >= m_paceCapUntil)
    {
        m_drainTimer.Cancel();
        m_drainPhase = PHASE_NORMAL;
        m_paceCapUntil = Seconds(0);
        m_paceCap = 0.0;
        if (tcb)
        {
            SetPacingRate(tcb);
        }
    }
}

void
TcpCopa::OnDrainTimer()
{
    const uint64_t rttMinUs = m_minRttFilter.GetBest();
    PollDrainExpiry(m_lastTcb, m_lastSrtt, rttMinUs);
}

void
TcpCopa::AbortDrain()
{
    m_drainTimer.Cancel();
    m_drainPhase = PHASE_NORMAL;
    m_paceCapUntil = Seconds(0);
    m_paceCap = 0.0;
    m_probeSaved = false;
}

bool
TcpCopa::DriveDrain(Ptr<TcpSocketState> tcb,
                    Time lrtt,
                    uint64_t rttMinUs,
                    uint64_t rttStandingUs,
                    uint64_t dqUs,
                    Time srtt)
{
    NS_LOG_FUNCTION(this << tcb << lrtt << rttMinUs << rttStandingUs << dqUs << srtt);
    const Time now = Simulator::Now();
    m_lastTcb = tcb;

    if (m_drainPhase == PHASE_DRAINING)
    {
        if (OnDrainAck(now, lrtt, rttMinUs, rttStandingUs, dqUs, srtt))
        {
            FinishDrain(tcb, rttMinUs);
            return false;
        }
        tcb->m_cWnd = CurrentDrainWindow(now, srtt);
        return true;
    }

    if (m_drainPhase != PHASE_NORMAL)
    {
        return false;
    }

    const bool takeHandover = m_handoverDrainPending;
    m_handoverDrainPending = false;
    if (takeHandover)
    {
        const double bwEst = EstimateBw(now, srtt);
        const double bdpEst = bwEst * (rttStandingUs * 1e-6);
        if (bwEst > 0.0 && tcb && tcb->m_cWnd.Get() < 0.75 * bdpEst)
        {
            StartDrain(tcb,
                       DRAIN_REFRESH_RTT_MIN,
                       false,
                       0,
                       now,
                       srtt,
                       rttMinUs,
                       rttStandingUs,
                       dqUs,
                       m_drainMin,
                       false);
            tcb->m_cWnd = CurrentDrainWindow(now, srtt);
            return true;
        }
    }

    const bool aligned = m_probePeriod.IsStrictlyPositive();
    const bool gentle = aligned && ProbingJitter() && m_gentleProbe;

    // 1. Initial post-slow-start gentle jitter calibration
    if (gentle && !m_slowStart && !m_firstProbeDone)
    {
        const Time maxDur = std::clamp(8 * srtt, MilliSeconds(200), MilliSeconds(600));
        StartDrain(tcb,
                   DRAIN_MEASURE_JITTER,
                   false,
                   0,
                   now,
                   srtt,
                   rttMinUs,
                   rttStandingUs,
                   dqUs,
                   maxDur,
                   false);
        tcb->m_cWnd = CurrentDrainWindow(now, srtt);
        return true;
    }

    // 2. Aligned slot MeasureJitter (measures j and refreshes RTTmin in one pass)
    if (gentle && InTimedProbe() && m_evaluatedSlot != Slot())
    {
        const uint64_t slot = Slot();
        m_evaluatedSlot = slot;
        const double dur =
            m_adaptiveProbes ? m_slotDuration : m_probeDuration.GetSeconds();
        const bool holdFull = dur > WantedDuration(srtt).GetSeconds() + 1e-3;
        StartDrain(tcb,
                   DRAIN_MEASURE_JITTER,
                   true,
                   slot,
                   now,
                   srtt,
                   rttMinUs,
                   rttStandingUs,
                   dqUs,
                   Seconds(std::max(dur, 0.0)),
                   holdFull);
        tcb->m_cWnd = CurrentDrainWindow(now, srtt);
        return true;
    }

    // 3. Aligned slot RefreshRttMin (triggered only on drift suspicion or board request)
    const uint64_t slot = DrainSlot();
    if (slot != UINT64_MAX && m_evaluatedSlot != slot)
    {
        m_evaluatedSlot = slot;
        ProbeBoard& board = Board();
        const uint32_t i = slot % 4;
        const bool boardReq = (board.drainRequests[i] == slot + 1);
        if (ShouldDrainSlot(now, slot, rttMinUs, boardReq))
        {
            board.drainRequests[i] = std::max(board.drainRequests[i], slot + 1);
            StartDrain(tcb,
                       DRAIN_REFRESH_RTT_MIN,
                       true,
                       slot,
                       now,
                       srtt,
                       rttMinUs,
                       rttStandingUs,
                       dqUs,
                       m_drainMin,
                       false);
            tcb->m_cWnd = CurrentDrainWindow(now, srtt);
            return true;
        }
    }

    return false;
}

bool
TcpCopa::LegacyProbeRecovery(Ptr<TcpSocketState> tcb, uint64_t rttMinUs)
{
    if (ProbingJitter() && m_probePeriod.IsStrictlyPositive() && m_gentleProbe)
    {
        m_probeSaved = false;
        return false;
    }
    const Time now = Simulator::Now();
    const bool probing = m_firstProbeDone && InTimedProbe();
    if (probing && !m_probeSaved)
    {
        m_probeSaved = true;
        m_savedCwnd = tcb->m_cWnd.Get();
        m_savedRate =
            m_rttStandingUs > 0 ? 2.0 * tcb->m_cWnd.Get() / (m_rttStandingUs * 1e-6) : 0;
        m_savedRttMinUs = rttMinUs;
        m_savedCongested = false;
    }
    else if (!probing && m_probeSaved)
    {
        m_probeSaved = false;
        m_trustedMinUs = rttMinUs;
        m_lastNearlyEmpty = now;
        const bool moved = RttMinChanged(m_savedRttMinUs * 1e-6, rttMinUs * 1e-6);
        if (m_recovery > 0.0 && !m_savedCongested && !moved)
        {
            const uint32_t restored = static_cast<uint32_t>(m_recovery * m_savedCwnd);
            tcb->m_cWnd = std::max(tcb->m_cWnd.Get(), restored);
            m_velocity = 1;
            m_direction = COPA_NONE;
            m_numTimesDirectionSame = 0;
            m_lastRecordedCwnd = 0;
            m_lastCwndRecordTime = Seconds(0);
            m_ackVotes = 0;
            m_prevVoteDir = 1;
            if (m_savedRate > 0)
            {
                m_paceCapUntil = now + std::max(m_lastSrtt, MilliSeconds(1));
                m_paceCap = m_savedRate;
            }
        }
    }
    return false;
}

void
TcpCopa::SetRateOps(Ptr<TcpRateOps> rateOps)
{
    NS_LOG_FUNCTION(this << rateOps);
    m_rateOps = rateOps;
}

std::string
TcpCopa::GetName() const
{
    return "TcpCopa";
}

bool
TcpCopa::HasCongControl() const
{
    return true;
}

uint32_t
TcpCopa::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    NS_LOG_FUNCTION(this << tcb << bytesInFlight);
    // Copa is delay-based and deliberately ignores loss. Returning the current
    // congestion window keeps the socket's recovery logic from inflating or
    // cutting the window: on fast-recovery exit the socket sets cwnd to the
    // returned slow-start threshold, and a delay-based algorithm wants that to
    // be the value it has already chosen.
    return tcb->m_cWnd.Get();
}

double
TcpCopa::CompetitiveCeiling() const
{
    return m_adaptive ? m_adaptiveMax : m_latencyFactor;
}

void
TcpCopa::OnPacketLost(uint32_t lostBytes)
{
    NS_LOG_FUNCTION(this << lostBytes);
    // In copa-auto-NC (ModeSwitching=false), non-persistent packet drops do not cancel drain
    // recovery; only ModeSwitching=true or RTO (CA_LOSS) cancels recovery.
    if (m_modeSwitching)
    {
        m_savedCongested = true;
        m_drainCongested = true;
    }
    // The default mode ignores loss, which makes Copa robust to non-congestive
    // (wireless, handover) losses. The competitive mode halves 1/delta, at
    // most once per RTT, like NewReno halves its window.
    if (!m_competitive || Simulator::Now() - m_lastDeltaDecrease < m_lastSrtt)
    {
        return;
    }
    m_delta = std::min(CompetitiveCeiling(), 2.0 * m_delta);
    m_lastDeltaDecrease = Simulator::Now();
    NS_LOG_DEBUG("loss in competitive mode, 1/delta=" << 1.0 / m_delta);
}

void
TcpCopa::UpdateMode(uint64_t dqUs, uint64_t rttMinUs, uint64_t rttMaxUs, Time srtt)
{
    NS_LOG_FUNCTION(this << dqUs << rttMinUs << rttMaxUs << srtt);
    if (!m_modeSwitching)
    {
        return;
    }
    Time now = Simulator::Now();
    const double dqS = dqUs * 1e-6;
    const double spanS =
        (rttMaxUs > rttMinUs) ? (rttMaxUs - std::min(rttMaxUs, rttMinUs)) * 1e-6 : 0.0;
    bool nearlyEmpty = false;
    if (m_adaptive)
    {
        if (m_jitter >= 0.0 && m_jitter < 0.015)
        {
            const double effDq = std::max(0.0, dqS - m_jitter);
            const double effSpan = std::max(0.0, spanS - m_jitter);
            nearlyEmpty = (effDq <= m_idleMargin.GetSeconds()) || (effDq < 0.1 * effSpan);
        }
        else
        {
            nearlyEmpty = (dqS < 0.1 * spanS);
        }
    }
    else
    {
        // "Nearly empty": queueing delay below 10% of the RTT oscillation of the
        // last four RTTs.
        nearlyEmpty = (dqS < 0.1 * spanS);
    }
    if (nearlyEmpty)
    {
        m_lastNearlyEmpty = now;
    }
    const bool competitive = (now - m_lastNearlyEmpty > 5 * srtt);
    if (competitive != m_competitive)
    {
        m_competitive = competitive;
        if (competitive && m_adaptive)
        {
            const double bwEst = EstimateBw(now, srtt);
            const double mss =
                (m_lastTcb && m_lastTcb->m_segmentSize > 0) ? m_lastTcb->m_segmentSize : 1448.0;
            const double bdpPkts = (bwEst > 0.0) ? (bwEst * (rttMinUs * 1e-6) / mss) : 0.0;
            if (bdpPkts > 150.0)
            {
                m_delta = std::clamp(m_delta, 0.02, CompetitiveCeiling());
            }
            else
            {
                m_delta = CompetitiveCeiling();
            }
        }
        else
        {
            // Both modes start from the default delta (AdaptiveDeltaMax for copa-auto).
            m_delta = CompetitiveCeiling();
        }
        NS_LOG_DEBUG((competitive ? "competitive" : "default") << " mode");
    }
}

void
TcpCopa::CheckAndUpdateDirection(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    Time now = Simulator::Now();
    Time srtt = m_lastSrtt;

    if (m_lastCwndRecordTime.IsZero() && m_lastRecordedCwnd == 0)
    {
        m_lastCwndRecordTime = now;
        m_lastRecordedCwnd = tcb->m_cWnd;
        return;
    }

    // Once per window (about one smoothed RTT).
    if (now - m_lastCwndRecordTime < srtt)
    {
        return;
    }

    CopaDirection_t newDirection = (tcb->m_cWnd > m_lastRecordedCwnd) ? COPA_UP : COPA_DOWN;
    if (newDirection != m_direction)
    {
        m_velocity = 1;
        m_numTimesDirectionSame = 0;
    }
    else
    {
        m_numTimesDirectionSame++;
        if (m_numTimesDirectionSame >= 3)
        {
            m_velocity = 2 * m_velocity;
        }
    }
    // As in the authors' implementation (genericCC): a step of v / (delta *
    // cwnd) must not exceed one packet per ACK, i.e. v <= delta * cwnd.
    const double cwndPkts = 1.0 * tcb->m_cWnd.Get() / tcb->m_segmentSize;
    while (m_velocity > 1 && m_velocity > m_delta * cwndPkts)
    {
        m_velocity /= 2;
    }
    m_direction = newDirection;
    m_lastCwndRecordTime = now;
    m_lastRecordedCwnd = tcb->m_cWnd;
}

void
TcpCopa::UpdateVelocityGenericCc(Ptr<TcpSocketState> tcb, bool increase, Time lrtt)
{
    NS_LOG_FUNCTION(this << tcb << increase << lrtt);
    const Time now = Simulator::Now();
    if (m_lastCwndRecordTime + lrtt < now)
    {
        if (m_prevVoteDir * m_ackVotes > 0)
        {
            m_velocity *= 2;
        }
        else
        {
            m_velocity = 1;
            m_prevVoteDir = -m_prevVoteDir;
        }
        m_lastCwndRecordTime = now;
        m_ackVotes = 0;
    }
    const double cwndPkts = 1.0 * tcb->m_cWnd.Get() / tcb->m_segmentSize;
    if (m_velocity > m_delta * cwndPkts)
    {
        m_velocity /= 2;
    }
    m_velocity = std::max<uint64_t>(m_velocity, 1);
    m_ackVotes += increase ? 1 : -1;
    m_direction = increase ? COPA_UP : COPA_DOWN;
}

void
TcpCopa::OnPacketAcked(Ptr<TcpSocketState> tcb, uint32_t ackedBytes)
{
    NS_LOG_FUNCTION(this << tcb << ackedBytes);

    Time lrtt = tcb->m_lastRtt.Get();
    if (!lrtt.IsStrictlyPositive())
    {
        return;
    }
    Time srtt = tcb->m_srtt.Get();
    if (!srtt.IsStrictlyPositive())
    {
        srtt = lrtt;
    }
    m_lastSrtt = srtt;
    m_lastTcb = tcb;
    PollDrainExpiry(tcb, srtt, m_minRttFilter.GetBest());

    uint64_t nowUs = Simulator::Now().GetMicroSeconds();

    // Windowed minimum RTT: an estimate of the propagation delay.
    m_minRttFilter.SetWindowLength(
        std::max<uint64_t>(m_minRttWindowLength.GetMicroSeconds(), 1));
    m_minRttFilter.Update(lrtt.GetMicroSeconds(), nowUs);
    uint64_t rttMinUs = m_minRttFilter.GetBest();

    // RTTstanding: minimum RTT over the last srtt/2, robust to ACK compression
    // and jitter.
    m_standingRttFilter.SetWindowLength(
        static_cast<uint64_t>(std::max<int64_t>(srtt.GetMicroSeconds() / 2, 1)));
    m_standingRttFilter.Update(lrtt.GetMicroSeconds(), nowUs);
    uint64_t rttStandingUs = m_standingRttFilter.GetBest();

    // RTTmax over the last four RTTs, for the "nearly empty" queue test.
    m_maxRttFilter.SetWindowLength(
        static_cast<uint64_t>(std::max<int64_t>(4 * srtt.GetMicroSeconds(), 1)));
    m_maxRttFilter.Update(lrtt.GetMicroSeconds(), nowUs);

    if (rttMinUs == 0 || rttStandingUs == 0)
    {
        return;
    }
    m_rttStandingUs = rttStandingUs;

    // Eq. (2) of the paper: dq = RTTstanding - RTTmin.
    uint64_t delayUs = rttStandingUs > rttMinUs ? rttStandingUs - rttMinUs : 0;

    if (m_adaptive)
    {
        if (!ProbingJitter() && !m_firstProbeDone)
        {
            m_jitter = std::min(m_jitterFloorMs * 1e-3, COPA_MAX_JITTER);
            m_firstProbeDone = true;
            m_alignedProbeSeen = true;
            m_stable = true;
        }
        const Time now = Simulator::Now();
        UpdateBwEstimate(now, ackedBytes, srtt, tcb->m_cWnd.Get(), m_lastAppLimited);
        if (m_drainPhase == PHASE_RECOVERING && m_paceCapUntil <= now)
        {
            m_drainPhase = PHASE_NORMAL;
        }
        if (m_paceCapUntil <= now)
        {
            m_paceCapUntil = Seconds(0);
        }
        const bool holding =
            (m_drainPhase == PHASE_DRAINING) ||
            (!m_competitive && (m_slowStart || m_deltaRun <= 0 || InGentleProbe()));
        if (holding)
        {
            // Do not mistake startup / initial delta or an intentional drain/probe
            // for a competing buffer-filler
            m_lastNearlyEmpty = now;
        }
        else
        {
            UpdateMode(delayUs, rttMinUs, m_maxRttFilter.GetBest(), srtt);
        }
        if (!m_slowStart && m_trustedMinUs == 0 && !ProbingJitter())
        {
            m_trustedMinUs = rttMinUs;
        }
        if (!m_competitive && m_trustedMinUs > 0)
        {
            // Idle test for skipping drains, against the RTTmin measured after the last probe or
            // drain: RTTmin itself drifts up with a busy queue, and every packet would look idle
            m_trustedMinUs = std::min<uint64_t>(m_trustedMinUs, lrtt.GetMicroSeconds());
            if (lrtt.GetSeconds() - m_trustedMinUs * 1e-6 <=
                std::max(m_jitter, 0.0) + m_idleMargin.GetSeconds())
            {
                m_lastIdle = now;
            }
        }
        const bool excluded = m_competitive || (m_drainPhase != PHASE_NORMAL) ||
                              (m_paceCapUntil > now);
        UpdateAdaptiveDelta(tcb, ackedBytes, lrtt, rttMinUs, delayUs, srtt, excluded);
    }
    else
    {
        UpdateMode(delayUs, rttMinUs, m_maxRttFilter.GetBest(), srtt);
    }
    if (!m_competitive)
    {
        // The default mode uses the configured delta (it may be set per flow
        // after the socket has been created), or the adaptive one.
        m_delta = m_latencyFactor;
        if (m_adaptive)
        {
            if (!m_slowStart && (InJitterProbe() || InTimedProbe()))
            {
                if (m_probePeriod.IsStrictlyPositive() && m_gentleProbe)
                {
                    if (m_deltaRun > 0)
                    {
                        m_delta = m_deltaRun;
                    }
                }
                else
                {
                    m_delta = m_adaptiveMax;
                }
            }
            else if (m_deltaRun > 0)
            {
                m_delta = m_deltaRun;
            }
            LegacyProbeRecovery(tcb, rttMinUs);
            if (DriveDrain(tcb, lrtt, rttMinUs, rttStandingUs, delayUs, srtt))
            {
                // Rate-based drain/calibration: smooth cwnd ramp + low-rate pacing pause Copa's
                // normal cwnd updates until drain exits.
                return;
            }
        }
    }
    else
    {
        // A probe/drain interrupted by the competitive mode is not restored
        AbortDrain();
        // Additive increase of 1/delta: one packet per RTT.
        const double ackedPkts = 1.0 * ackedBytes / tcb->m_segmentSize;
        const double cwndPkts = std::max(1.0, 1.0 * tcb->m_cWnd.Get() / tcb->m_segmentSize);
        m_delta = 1.0 / (1.0 / m_delta + ackedPkts / cwndPkts);
    }

    bool increaseCwnd;
    if (delayUs == 0)
    {
        // No queueing delay measured: always increase.
        increaseCwnd = true;
    }
    else
    {
        const double targetRate = (1.0 * tcb->m_segmentSize * 1e6) / (m_delta * delayUs);
        const double currentRate = (1.0 * tcb->m_cWnd.Get() * 1e6) / rttStandingUs;
        increaseCwnd = (targetRate >= currentRate);
    }

    if (!(increaseCwnd && m_slowStart))
    {
        if (m_genericCcVelocity)
        {
            UpdateVelocityGenericCc(tcb, increaseCwnd, lrtt);
        }
        else
        {
            CheckAndUpdateDirection(tcb);
        }
    }

    if (increaseCwnd)
    {
        if (m_slowStart)
        {
            // One packet per acknowledged packet (as genericCC): cwnd doubles
            // per RTT and is clocked by the ACKs, whatever the RTT estimate.
            const uint64_t newCwnd = static_cast<uint64_t>(tcb->m_cWnd.Get()) + ackedBytes;
            tcb->m_cWnd = std::min<uint64_t>(newCwnd, 1ull << 31);
        }
        else
        {
            // cwnd += v / (delta * cwnd) packets per acknowledged packet.
            uint64_t addition = static_cast<uint64_t>(
                (1.0 * ackedBytes * tcb->m_segmentSize * m_velocity) /
                (m_delta * tcb->m_cWnd.Get()));
            uint64_t newCwnd = static_cast<uint64_t>(tcb->m_cWnd.Get()) + addition;
            tcb->m_cWnd = std::min<uint64_t>(newCwnd, 1ull << 31);
        }
    }
    else
    {
        uint64_t reduction = static_cast<uint64_t>(
            (1.0 * ackedBytes * tcb->m_segmentSize * m_velocity) /
            (m_delta * tcb->m_cWnd.Get()));
        m_slowStart = false;
        uint64_t room = (tcb->m_cWnd.Get() > m_minCwnd) ? (tcb->m_cWnd.Get() - m_minCwnd) : 0;
        tcb->m_cWnd = tcb->m_cWnd.Get() - std::min<uint64_t>(reduction, room);
    }

    if (tcb->m_cWnd.Get() < m_minCwnd)
    {
        tcb->m_cWnd = m_minCwnd;
    }

    NS_LOG_DEBUG("delay=" << delayUs << "us"
                          << " rttMin=" << rttMinUs << "us"
                          << " rttStanding=" << rttStandingUs << "us"
                          << " velocity=" << m_velocity << " delta=" << m_delta
                          << " competitive=" << m_competitive << " cwnd=" << tcb->m_cWnd.Get());
}

void
TcpCopa::SetPacingRate(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);

    if (!tcb->m_pacing)
    {
        tcb->m_pacing = true;
    }

    double bps;
    if (m_drainPhase == PHASE_DRAINING && m_drainRate > 0.0)
    {
        bps = 8.0 * m_drainRate;
    }
    else
    {
        if (m_rttStandingUs == 0)
        {
            return;
        }
        // Twice cwnd / RTTstanding, to accommodate pacing imperfections (Sec. 2.1).
        bps = 2.0 * (8.0 * tcb->m_cWnd.Get()) / (m_rttStandingUs * 1e-6);
        if (m_paceCapUntil > Simulator::Now() && m_paceCap > 0.0)
        {
            // After a fast recovery: the rate from before the probe, for one smoothed RTT
            bps = std::min(bps, 8.0 * m_paceCap);
        }
    }

    DataRate rate(bps);
    if (rate > tcb->m_maxPacingRate)
    {
        rate = tcb->m_maxPacingRate;
    }
    tcb->m_pacingRate = rate;
}

void
TcpCopa::CongControl(Ptr<TcpSocketState> tcb,
                     const TcpRateOps::TcpRateConnection& rc,
                     const TcpRateOps::TcpRateSample& rs)
{
    NS_LOG_FUNCTION(this << tcb << rs);

    if (!m_isInitialized)
    {
        m_minCwnd = COPA_MIN_CWND_SEGMENTS * tcb->m_segmentSize;
        Reset();
        m_isInitialized = true;
    }
    m_lastTcb = tcb;
    m_lastAppLimited = rs.m_isAppLimited;

    if (rs.m_ackedSacked > 0)
    {
        OnPacketAcked(tcb, rs.m_ackedSacked);
    }

    if (rs.m_bytesLoss > 0)
    {
        OnPacketLost(rs.m_bytesLoss);
    }

    SetPacingRate(tcb);
}

void
TcpCopa::CongestionStateSet(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCongState_t newState)
{
    NS_LOG_FUNCTION(this << tcb << newState);
    if (newState == TcpSocketState::CA_OPEN && !m_isInitialized)
    {
        m_minCwnd = COPA_MIN_CWND_SEGMENTS * tcb->m_segmentSize;
        Reset();
        tcb->m_pacing = true;
        m_lastTcb = tcb;
        m_isInitialized = true;
    }
    else if (newState == TcpSocketState::CA_LOSS && m_isInitialized)
    {
        // Retransmission timeout: the socket has set cwnd to one segment. The
        // paper does not cover this case; as TCP does after a timeout (RFC
        // 5681), restart the slow start, which doubles cwnd per RTT until
        // the current rate exceeds the target rate.
        AbortDrain();
        m_slowStart = true;
        m_savedCongested = true;
        m_drainCongested = true;
        m_velocity = 1;
        m_direction = COPA_NONE;
        m_numTimesDirectionSame = 0;
        m_lastRecordedCwnd = 0;
        m_lastCwndRecordTime = Seconds(0);
        m_ackVotes = 0;
        m_prevVoteDir = 1;
    }
}

void
TcpCopa::CwndEvent(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCAEvent_t event)
{
    NS_LOG_FUNCTION(this << tcb << event);
    // Copa has no loss-based window reduction, so no event handling is required.
}

uint32_t
TcpCopa::GetDirection() const
{
    return static_cast<uint32_t>(m_direction);
}

uint64_t
TcpCopa::GetVelocity() const
{
    return m_velocity;
}

double
TcpCopa::GetDelta() const
{
    return m_delta;
}

bool
TcpCopa::IsCompetitive() const
{
    return m_competitive;
}

Ptr<TcpCongestionOps>
TcpCopa::Fork()
{
    return CopyObject<TcpCopa>(this);
}

} // namespace ns3
