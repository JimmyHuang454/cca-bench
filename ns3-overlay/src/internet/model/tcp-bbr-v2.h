/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * BBRv2-style congestion control.
 *
 * This is a BBRv2-style model for ns-3. It keeps the BBRv1 model of the
 * bottleneck (max bandwidth filter and min RTT) and adds the two mechanisms
 * that define BBRv2:
 *
 *  - an explicit upper/lower bound on the amount of data in flight
 *    (inflight_hi / inflight_lo) that is learned from loss and ECN, so that
 *    the window is never pushed past the point where the path starts dropping;
 *  - a ProbeBW gain cycle with four phases (UP, DOWN, CRUISE, REFILL) that
 *    respects those bounds instead of blindly ramping up.
 *
 * The reference is the IETF draft "BBR Congestion Control" and the reference
 * implementations in Linux and mvfst. This model is not a line-by-line port of
 * the draft; it implements the mechanisms that drive the comparison against
 * CUBIC / BBRv1 / Copa and is documented as such.
 */

#ifndef TCP_BBR_V2_H
#define TCP_BBR_V2_H

#include "tcp-congestion-ops.h"
#include "windowed-filter.h"

#include "ns3/data-rate.h"
#include "ns3/random-variable-stream.h"
#include "ns3/traced-value.h"

namespace ns3
{

/**
 * @ingroup congestionOps
 *
 * @brief BBRv2-style congestion control algorithm.
 */
class TcpBbrV2 : public TcpCongestionOps
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    TcpBbrV2();

    /**
     * @brief Copy constructor.
     * @param sock object to copy.
     */
    TcpBbrV2(const TcpBbrV2& sock);

    ~TcpBbrV2() override;

    /**
     * @brief BBRv2 modes.
     */
    enum BbrV2Mode_t
    {
        BBRV2_STARTUP,   //!< Ramp up to estimate the bottleneck bandwidth
        BBRV2_DRAIN,     //!< Drain the queue built during startup
        BBRV2_PROBE_BW,  //!< Probe for more bandwidth around the estimate
        BBRV2_PROBE_RTT, //!< Reduce inflight to refresh the min RTT
    };

    /**
     * @brief Phases of the ProbeBW gain cycle.
     */
    enum ProbeBwPhase_t
    {
        PROBE_UP = 0,  //!< Probe upwards for more bandwidth
        PROBE_DOWN,    //!< Drain the queue created by the up phase
        PROBE_CRUISE,  //!< Cruise at the estimated bandwidth
        PROBE_REFILL,  //!< Refill the pipe before the next up phase
    };

    typedef WindowedFilter<DataRate,
                           MaxFilter<DataRate>,
                           uint32_t,
                           uint32_t>
        MaxBandwidthFilter_t; //!< Max bandwidth windowed filter

    /**
     * @brief Literal names of the BBRv2 modes (for logging).
     */
    static const char* const BbrV2ModeName[BBRV2_PROBE_RTT + 1];

    /**
     * @brief Literal names of the ProbeBW phases (for logging).
     */
    static const char* const ProbeBwPhaseName[PROBE_REFILL + 1];

    /**
     * @brief Assign a fixed random variable stream number.
     * @param stream first stream index to use
     */
    virtual void SetStream(uint32_t stream);

    std::string GetName() const override;
    bool HasCongControl() const override;
    void CongControl(Ptr<TcpSocketState> tcb,
                     const TcpRateOps::TcpRateConnection& rc,
                     const TcpRateOps::TcpRateSample& rs) override;
    void CongestionStateSet(Ptr<TcpSocketState> tcb,
                            const TcpSocketState::TcpCongState_t newState) override;
    void CwndEvent(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCAEvent_t event) override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    void SetRateOps(Ptr<TcpRateOps> rateOps) override;
    Ptr<TcpCongestionOps> Fork() override;

    /**
     * @brief Gets the current BBRv2 mode.
     * @return the mode.
     */
    uint32_t GetBbrState() const;

    /**
     * @brief Gets the current ProbeBW phase.
     * @return the phase.
     */
    uint32_t GetProbeBwPhase() const;

    /**
     * @brief Gets the current pacing gain.
     * @return the pacing gain.
     */
    double GetPacingGain() const;

    /**
     * @brief Gets the current cwnd gain.
     * @return the cwnd gain.
     */
    double GetCwndGain() const;

    /**
     * @brief Gets the current inflight_hi bound, in bytes.
     * @return the bound (0 means unbounded).
     */
    uint32_t GetInflightHi() const;

    /**
     * @brief Gets the current inflight_lo bound, in bytes.
     * @return the bound (0 means no constraint).
     */
    uint32_t GetInflightLo() const;

    /**
     * @brief Select the BBR version to emulate.
     *
     * Version 3 adds the bw_lo minimum-bandwidth filter used to bound pacing
     * during recovery and a two-phase ProbeBW cycle (PROBE_UP/PROBE_DOWN).
     *
     * @param version 2 or 3
     */
    void SetBbrVersion(int version)
    {
        m_bbrVersion = version;
    }

  private:
    void InitRoundCounting();
    void InitFullPipe();
    void InitPacingRate(Ptr<TcpSocketState> tcb);
    void EnterStartup();
    void EnterDrain();
    void EnterProbeBW();
    void EnterProbeRTT();
    void ExitProbeRTT();
    void SetBbrState(BbrV2Mode_t state);
    void SetProbeBwPhase(Ptr<TcpSocketState> tcb, ProbeBwPhase_t phase);

    void UpdateRound(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void UpdateRTprop(Ptr<TcpSocketState> tcb);
    void UpdateBottleneckBandwidth(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void UpdateInflightBounds(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void CheckFullPipe(const TcpRateOps::TcpRateSample& rs);
    void CheckCyclePhase(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void CheckDrain(Ptr<TcpSocketState> tcb);
    void CheckProbeRTT(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void HandleProbeRTT(Ptr<TcpSocketState> tcb);
    void HandleRestartFromIdle(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    void UpdateModelAndState(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void UpdateControlParameters(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);

    uint32_t InFlight(Ptr<TcpSocketState> tcb, double gain) const;
    uint32_t TargetCwnd(Ptr<TcpSocketState> tcb, double gain) const;
    uint32_t EffectiveInflightHi() const;
    bool InLossRecovery() const;
    bool IsNextCyclePhase(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs) const;
    void ModulateCwndForProbeRTT(Ptr<TcpSocketState> tcb) const;
    bool ModulateCwndForRecovery(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void SaveCwnd(Ptr<const TcpSocketState> tcb);
    void RestoreCwnd(Ptr<TcpSocketState> tcb);
    void SetPacingRate(Ptr<TcpSocketState> tcb, double gain);
    void SetSendQuantum(Ptr<TcpSocketState> tcb);
    void SetCwnd(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void ReduceInflightBounds(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    /**
     * Linux BBRv2/v3 loss response, used when LinuxLossResponse is true.
     *
     * @param tcb the socket state
     * @param rs the rate sample
     * @param lossRate lost / prior inflight of this sample
     * @param prior inflight before the sample
     * @param inflightLatest inflight that was not lost
     */
    void ReduceInflightBoundsLinux(Ptr<TcpSocketState> tcb,
                                   const TcpRateOps::TcpRateSample& rs,
                                   double lossRate,
                                   uint32_t prior,
                                   uint32_t inflightLatest);

    // Linux-style ProbeBW cycle (DOWN, CRUISE, REFILL, UP), used in Linux mode.
    void StartProbeDown();
    void StartProbeCruise();
    void StartProbeRefill();
    void StartProbeUp();
    bool IsProbingBandwidth() const;
    bool IsTimeToProbe(Ptr<TcpSocketState> tcb) const;
    void CheckCyclePhaseLinux(Ptr<TcpSocketState> tcb, const TcpRateOps::TcpRateSample& rs);
    void AdaptLowerBounds(Ptr<TcpSocketState> tcb);
    void ResetLowerBounds();
    uint32_t BwFilterTime() const;
    uint32_t BoundCwndForModel(uint32_t cwnd) const;

    // Model
    BbrV2Mode_t m_state{BBRV2_STARTUP};   //!< Current mode
    ProbeBwPhase_t m_phase{PROBE_UP};     //!< Current ProbeBW phase
    MaxBandwidthFilter_t m_maxBwFilter;   //!< Max bandwidth filter
    uint32_t m_bandwidthWindowLength{10}; //!< Bandwidth filter window, in rounds
    int m_bbrVersion{2};                  //!< Emulated BBR version (2 or 3)
    WindowedFilter<DataRate, MinFilter<DataRate>, uint32_t, uint32_t>
        m_bwLoFilter; //!< Minimum bandwidth filter (BBRv3)
    TracedValue<double> m_pacingGain{0};  //!< Pacing gain
    TracedValue<double> m_cWndGain{0};    //!< Congestion window gain
    double m_highGain{2.89};              //!< Startup gain
    bool m_isPipeFilled{false};           //!< Whether startup has filled the pipe
    uint32_t m_minPipeCwnd{0};            //!< Minimum pipe cwnd (4 * MSS)

    uint32_t m_roundCount{0};          //!< Packet-timed round trip count
    bool m_roundStart{false};          //!< True once per round trip
    uint32_t m_nextRoundDelivered{0};  //!< End of the current round trip

    Time m_probeRttDuration{MilliSeconds(200)}; //!< Minimum time in ProbeRTT
    Time m_probeRttDoneStamp;                   //!< Time to leave ProbeRTT
    bool m_probeRttRoundDone{false};            //!< Round trip completed in ProbeRTT
    bool m_packetConservation{false};           //!< Packet conservation mode
    uint32_t m_priorCwnd{0};                    //!< Last-known good cwnd
    bool m_idleRestart{false};                  //!< Restarting from idle
    uint32_t m_targetCWnd{0};                   //!< Target congestion window

    DataRate m_fullBandwidth{0};      //!< Last full bandwidth estimate
    uint32_t m_fullBandwidthCount{0}; //!< Rounds without bandwidth growth

    TracedValue<Time> m_minRtt{Time::Max()}; //!< Windowed minimum RTT
    Time m_minRttStamp;                      //!< Time of the min RTT sample
    Time m_minRttFilterLen{Seconds(10)};     //!< Min RTT filter length
    bool m_minRttExpired{false};             //!< Whether the min RTT has expired

    uint32_t m_sendQuantum{0}; //!< Maximum send aggregate
    Time m_cycleStamp;         //!< Time of the current cycle phase

    // BBRv2 inflight bounds
    uint32_t m_inflightHi{0};     //!< Upper bound on inflight (0 = unbounded)
    uint32_t m_inflightLo{0};     //!< Lower bound used while draining
    uint32_t m_inflightLatest{0}; //!< Most recent inflight sample
    uint32_t m_inflightHiMax{0};  //!< Largest inflight_hi seen this cycle
    double m_lossThreshold{0.02}; //!< Loss rate above which inflight_hi is cut
    double m_ecnThreshold{0.5};   //!< ECN rate above which inflight_hi is cut
    double m_beta{0.3};            //!< Loss rate above which cwnd is reduced
    uint32_t m_roundsSinceLoss{0}; //!< Rounds since the last loss event
    bool m_lossRecovery{false};    //!< True while draining after a loss event
    bool m_linuxLoss{false};       //!< Use the Linux BBRv2/v3 loss response
    DataRate m_bwLoLinux{0};       //!< bw_lo of the Linux loss response (0 = unbounded)
    uint32_t m_lossRecoveryRounds{0}; //!< Rounds spent in the loss recovery state

    // Linux-mode congestion signals and cycle
    bool m_lossInRound{false};         //!< Loss seen in the current round
    DataRate m_bwLatest{0};            //!< Max delivery rate in the current round
    uint32_t m_inflightLatestRound{0}; //!< Max delivered volume in the current round
    Time m_probeDownStamp;             //!< Start of the current PROBE_DOWN
    Time m_probeWait;                  //!< Wall-clock wait before the next bandwidth probe
    uint32_t m_roundsSinceProbe{0};    //!< Rounds since the last PROBE_DOWN start
    uint32_t m_refillRound{0};         //!< Round in which PROBE_REFILL started
    uint32_t m_cycleCount{0};          //!< ProbeBW cycles (time base of the bw filter)

    bool m_isInitialized{false}; //!< Runtime state initialized
    Ptr<UniformRandomVariable> m_uv{nullptr}; //!< Random variable for phase start
    uint64_t m_delivered{0};                  //!< Total bytes delivered
    Ptr<TcpRateOps> m_rateOps;                //!< Rate operations
};

} // namespace ns3

#endif // TCP_BBR_V2_H
