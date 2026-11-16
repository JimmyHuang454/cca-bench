/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copa congestion control.
 *
 * This is an implementation in ns-3 of the Copa congestion control algorithm
 * described in:
 *
 *   Venkat Arun and Hari Balakrishnan, "Copa: Practical Delay-Based
 *   Congestion Control for the Internet", USENIX NSDI 2018.
 *
 * The implementation follows Section 2 of the paper: the queueing delay is
 * RTTstanding - RTTmin, every ACK moves the congestion window towards the
 * target rate 1 / (delta * dq) with a velocity that doubles once per window
 * after three windows in the same direction, packets are paced at
 * 2 * cwnd / RTTstanding, and the sender switches between a default mode
 * (delta = 0.5, losses ignored) and a competitive mode (AIMD on 1 / delta)
 * when it detects a buffer-filling competitor.
 */

#ifndef TCP_COPA_H
#define TCP_COPA_H

#include "tcp-congestion-ops.h"
#include "windowed-filter.h"

#include "ns3/data-rate.h"
#include "ns3/event-id.h"
#include "ns3/traced-callback.h"
#include "ns3/traced-value.h"

namespace ns3
{

/**
 * @ingroup congestionOps
 *
 * @brief Copa congestion control algorithm.
 *
 * Copa uses a target rate of 1 / (delta * dq), where dq = RTTstanding - RTTmin
 * is the measured queueing delay and delta is a latency-sensitivity factor.
 * Every acknowledged packet moves the congestion window by v / (delta * cwnd)
 * packets towards the target rate, where the velocity v doubles once per window
 * after the direction has stayed the same for three windows.
 *
 * In the default mode (delta = 0.5) losses are ignored. If the queue has not
 * been nearly empty (dq < 0.1 * (RTTmax - RTTmin), RTTmax over the last four
 * RTTs) at least once in the last five RTTs, a buffer-filling flow shares the
 * bottleneck and Copa switches to the competitive mode, where 1 / delta
 * follows a NewReno-like AIMD: +1 packet per RTT, halved on loss (at most once
 * per RTT), with delta <= 0.5.
 */
class TcpCopa : public TcpCongestionOps
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    TcpCopa();

    /**
     * @brief Copy constructor.
     * @param sock object to copy.
     */
    TcpCopa(const TcpCopa& sock);

    ~TcpCopa() override;

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
     * @brief Gets the current direction of the window adjustment.
     * @return 0 (none), 1 (up) or 2 (down).
     */
    uint32_t GetDirection() const;

    /**
     * @brief Gets the current velocity.
     * @return the velocity (packet multiplier per round trip).
     */
    uint64_t GetVelocity() const;

    /**
     * @brief Gets the current delta (latency factor in use).
     * @return delta; 0.5 in the default mode, lower in the competitive mode.
     */
    double GetDelta() const;

    /**
     * @brief Whether Copa is in the competitive mode.
     * @return true in the competitive mode, false in the default mode.
     */
    bool IsCompetitive() const;

    /**
     * TracedCallback signature of the adaptive-delta estimator, called once
     * per round trip.
     *
     * @param [in] jitterS jitter floor j [s]
     * @param [in] ratioS ratio c / mu of the burstiness to the capacity [s]
     * @param [in] rho total utilization of the bottleneck (last estimate)
     * @param [in] baseRttS RTT without self-induced queueing R0' [s]
     * @param [in] dqTargetS power-optimal queueing delay dq* [s] (0 until known)
     * @param [in] delta delta in use
     */
    typedef void (*AdaptiveEstimateTracedCallback)(double jitterS,
                                                   double ratioS,
                                                   double rho,
                                                   double baseRttS,
                                                   double dqTargetS,
                                                   double delta);

  private:
    /**
     * @brief Direction of the last congestion window update.
     */
    enum CopaDirection_t
    {
        COPA_NONE = 0, //!< No direction recorded yet
        COPA_UP,       //!< Window is increasing
        COPA_DOWN,     //!< Window is decreasing
    };

    /**
     * Windowed minimum RTT filter. Samples and timestamps are in microseconds,
     * which is what the reference implementation uses.
     */
    typedef WindowedFilter<uint64_t, MinFilter<uint64_t>, uint64_t, uint64_t> RttFilter_t;

    /**
     * @brief Handle a set of acknowledged packets.
     * @param tcb internal congestion state
     * @param ackedBytes number of bytes acknowledged by the current ACK
     */
    void OnPacketAcked(Ptr<TcpSocketState> tcb, uint32_t ackedBytes);

    /**
     * @brief Handle a loss event: ignored in the default mode, halves 1 / delta
     * in the competitive mode. Copa never reduces cwnd directly on loss.
     * @param lostBytes number of bytes reported lost
     */
    void OnPacketLost(uint32_t lostBytes);

    /**
     * @brief Upper bound and fallback delta of the competitive mode (LatencyFactor for fixed
     * Copa, AdaptiveDeltaMax for copa-auto).
     * @return ceiling delta
     */
    double CompetitiveCeiling() const;

    /**
     * @brief Detect a buffer-filling competitor and switch mode (Section 2.2).
     * @param dqUs queueing delay RTTstanding - RTTmin [us]
     * @param rttMinUs long-term minimum RTT [us]
     * @param rttMaxUs maximum RTT over the last four RTTs [us]
     * @param srtt smoothed RTT
     */
    void UpdateMode(uint64_t dqUs, uint64_t rttMinUs, uint64_t rttMaxUs, Time srtt);

    /**
     * @brief Update direction/velocity once per round trip.
     * @param tcb internal congestion state
     */
    void CheckAndUpdateDirection(Ptr<TcpSocketState> tcb);

    /**
     * @brief Velocity update of the authors' implementation (genericCC,
     * markoviancc.cc): the direction of a round is the majority of the
     * per-ACK decisions, and the velocity doubles as soon as two consecutive
     * rounds have the same direction.
     * @param tcb internal congestion state
     * @param increase decision of this ACK
     * @param lrtt latest RTT sample (length of a round)
     */
    void UpdateVelocityGenericCc(Ptr<TcpSocketState> tcb, bool increase, Time lrtt);

    /**
     * @brief Set the pacing rate to 2 * cwnd / RTTstanding.
     * @param tcb internal congestion state
     */
    void SetPacingRate(Ptr<TcpSocketState> tcb);

    /**
     * @brief Reset the per-connection state.
     */
    void Reset();

    /**
     * @brief Online choice of delta from the wireless model (adaptive mode).
     * Each flow runs it on its own, without knowing how many Copa flows or
     * how much background traffic share the bottleneck.
     *
     * Measurements are averaged over half-periods (AdaptiveHalfRounds round
     * trips at one delta, the first AdaptiveSettleRounds skipped):
     * - total utilization rho of the bottleneck: the link is idle a fraction
     *   1 - rho of the time, estimated by the fraction of the flow's packets
     *   that did not queue (RTT - RTTmin <= j + AdaptiveIdleMargin);
     * - jitter floor j: mean dq at low load, minimum over the low-load probes
     *   (two half-periods at AdaptiveDeltaMax every AdaptiveProbeEvery);
     * - ratio k = c / mu: from dq = j + k rho / (1 - rho), k = (dq - j)
     *   (1 - rho) / rho, exponential average in log scale.
     *
     * The power-optimal queueing delay dq* = j + sqrt(k R0') (R0' = RTTmin +
     * j) depends neither on the number of flows nor on the background. Each
     * flow moves its delta towards 1 / (lambda dq*), which at Copa's
     * equilibrium (lambda dq = 1 / delta) is delta (dq / dq*):
     * delta <- delta (dq / dq*)^g once per half-period, g = AdaptiveGainFar
     * when dq / dq* is beyond AdaptiveFarRatio (or its inverse), otherwise
     * AdaptiveGain.
     *
     * @param tcb internal congestion state
     * @param ackedBytes number of bytes acknowledged by the current ACK
     * @param lrtt RTT of the last acknowledged segment
     * @param rttMinUs long-term minimum RTT [us]
     * @param dqUs queueing delay RTTstanding - RTTmin [us]
     * @param srtt smoothed RTT
     */
    void UpdateAdaptiveDelta(Ptr<TcpSocketState> tcb,
                             uint32_t ackedBytes,
                             Time lrtt,
                             uint64_t rttMinUs,
                             uint64_t dqUs,
                             Time srtt,
                             bool excluded);

    /**
     * @brief Kind of queue drain / calibration driven by DrainController.
     */
    enum DrainKind
    {
        DRAIN_REFRESH_RTT_MIN = 0, //!< Short rate-based drain to refresh RTTmin
        DRAIN_MEASURE_JITTER       //!< Queue drain + post-idle sampling to measure j
    };

    /**
     * @brief Lifecycle phase of the rate-based DrainController.
     */
    enum DrainPhase
    {
        PHASE_NORMAL = 0, //!< Normal Copa control
        PHASE_DRAINING,   //!< Active drain / calibration
        PHASE_RECOVERING  //!< Post-drain pacing rate cap for 1 * srtt
    };

    /**
     * @brief Record aligned-probe outcome for adaptive jitter settling & backoff.
     * @param probeDq measured queueing delay during the probe [s]
     * @param rttMinUs current minimum RTT [us]
     */
    void RecordAlignedProbe(double probeDq, uint64_t rttMinUs);

    /**
     * @brief Complete a gentle MeasureJitter pass (initial or aligned slot).
     * @param hasSlot true when attached to an aligned slot
     * @param slot aligned slot index
     * @param hasProbeDq true when a valid post-idle jitter sample was collected
     * @param probeDq measured jitter floor [s]
     * @param rttMinUs current minimum RTT [us]
     */
    void FinishGentleProbe(bool hasSlot,
                           uint64_t slot,
                           bool hasProbeDq,
                           double probeDq,
                           uint64_t rttMinUs);

    /**
     * @brief Update the 2-bucket windowed maximum delivery bandwidth estimator.
     */
    void UpdateBwEstimate(Time now,
                          uint32_t ackedBytes,
                          Time srtt,
                          uint32_t cwnd,
                          bool appLimited);

    /**
     * @brief Return recent estimated delivery bandwidth [bytes/s], or 0 if unavailable/stale.
     */
    double EstimateBw(Time now, Time srtt) const;

    /**
     * @brief Return whether slot should trigger a RefreshRttMin drain.
     */
    bool ShouldDrainSlot(Time now,
                         uint64_t slot,
                         uint64_t rttMinUs,
                         bool boardRequested) const;

    /**
     * @brief Start a rate-based queue drain or jitter measurement.
     */
    void StartDrain(Ptr<TcpSocketState> tcb,
                    DrainKind kind,
                    bool hasSlot,
                    uint64_t slot,
                    Time now,
                    Time srtt,
                    uint64_t rttMinUs,
                    uint64_t rttStandingUs,
                    uint64_t dqUs,
                    Time maxDuration,
                    bool holdFull);

    /**
     * @brief Compute smooth 1 * srtt cwnd ramp toward 0.90 * BDP during active drain.
     */
    uint32_t CurrentDrainWindow(Time now, Time srtt) const;

    /**
     * @brief Feed an ACK sample during active drain; finishes early on baseline/plateau.
     * @return true if the drain has completed
     */
    bool OnDrainAck(Time now,
                    Time lrtt,
                    uint64_t rttMinUs,
                    uint64_t rttStandingUs,
                    uint64_t dqUs,
                    Time srtt);

    /**
     * @brief Check and apply drain/recovery hard-deadline expiry on incoming events.
     */
    void PollDrainExpiry(Ptr<TcpSocketState> tcb, Time srtt, uint64_t rttMinUs);

    /**
     * @brief Complete an active drain and transition to recovery (or Normal).
     */
    void FinishDrain(Ptr<TcpSocketState> tcb, uint64_t rttMinUs);

    /**
     * @brief Abort any active drain/recovery without window restoration.
     */
    void AbortDrain();

    /** @brief Timer callback for hard deadline expiry between ACKs. */
    void OnDrainTimer();

    /** @brief Schedule (or cancel) m_drainTimer to expire at until. */
    void ScheduleDrainTimer(Time until);

    /**
     * @brief Drive rate-based RefreshRttMin and gentle MeasureJitter drains.
     * @return true while an active drain pauses Copa's normal cwnd law
     */
    bool DriveDrain(Ptr<TcpSocketState> tcb,
                    Time lrtt,
                    uint64_t rttMinUs,
                    uint64_t rttStandingUs,
                    uint64_t dqUs,
                    Time srtt);

    /**
     * @brief Legacy fast recovery around delta_max aligned probes (when AdaptiveGentleProbe=false).
     */
    bool LegacyProbeRecovery(Ptr<TcpSocketState> tcb, uint64_t rttMinUs);

    /** @return whether j is probed online (AdaptiveJitterFloorMs < 0) */
    bool ProbingJitter() const;
    /** @return whether a gentle BDP-hold probe is currently active */
    bool InGentleProbe() const;

    /** @return index of the aligned slot containing now */
    uint64_t Slot() const;
    /** @return position of now in the aligned slot [s] */
    double Phase() const;
    /** @param srtt smoothed RTT @return length of the probe this flow needs */
    Time WantedDuration(Time srtt) const;
    /** @param level backoff level @return slots per fallback period */
    uint64_t FallbackEvery(uint32_t level) const;
    /** @return highest backoff level */
    uint32_t MaxFallbackLevel() const;
    /** @param slot aligned slot @return whether this flow wants a probe in it */
    bool WantsProbe(uint64_t slot) const;
    /** @param a first RTTmin [s] @param b second RTTmin [s] @return whether it moved notably */
    bool RttMinChanged(double a, double b) const;
    /** Start a new estimate of the jitter floor with the next aligned probe. */
    void Unsettle();
    /** @return the slot to drain now, or UINT64_MAX */
    uint64_t DrainSlot() const;

    /**
     * Probe and drain requests shared by the flows of one AdaptiveBoardGroup, indexed by slot
     * modulo 4: requests/drainRequests hold slot + 1, lengths the longest probe [us] needed
     * in that slot, tagged with slot + 1.
     */
    struct ProbeBoard
    {
        uint64_t requests[4]{0, 0, 0, 0};
        uint64_t drainRequests[4]{0, 0, 0, 0};
        uint64_t lengthSlot[4]{0, 0, 0, 0};
        uint64_t lengthUs[4]{0, 0, 0, 0};
    };

    /** @return the board of this flow's group */
    ProbeBoard& Board() const;

    /**
     * @brief Whether the current half-period is a low-load probe of the jitter floor.
     * @return true during a low-load probe
     */
    bool InJitterProbe() const;

    /**
     * @brief Whether now is inside a time-aligned low-load probe
     * (AdaptiveProbePeriod > 0). All flows of a host share the clock, so they
     * probe at the same time and the bottleneck queue empties.
     * @return true inside the probe window
     */
    bool InTimedProbe() const;

    /**
     * Windowed maximum RTT filter (microseconds), for the "nearly empty" test.
     */
    typedef WindowedFilter<uint64_t, MaxFilter<uint64_t>, uint64_t, uint64_t> RttMaxFilter_t;

    double m_latencyFactor{0.5};            //!< Delta of the default mode
    Time m_minRttWindowLength{Seconds(10)}; //!< Length of the min RTT filter window
    uint32_t m_minCwnd{0};                  //!< Minimum congestion window (4 * MSS)
    bool m_modeSwitching{true};             //!< Enable the competitive mode
    bool m_genericCcVelocity{false};        //!< Velocity rule of genericCC instead of the paper
    int64_t m_ackVotes{0};                  //!< Increase minus decrease decisions in this round
    int m_prevVoteDir{1};                   //!< Direction of the previous round (+1 or -1)

    RttFilter_t m_minRttFilter;      //!< Long term minimum RTT (propagation delay)
    RttFilter_t m_standingRttFilter; //!< Short term minimum RTT (standing queue)
    RttMaxFilter_t m_maxRttFilter;   //!< Maximum RTT over the last four RTTs
    uint64_t m_rttStandingUs{0};     //!< Last RTTstanding [us], used for pacing

    double m_delta{0.5};               //!< Delta in use
    bool m_competitive{false};         //!< True in the competitive mode
    Time m_lastNearlyEmpty;            //!< Last time the queue was nearly empty
    Time m_lastDeltaDecrease;          //!< Last halving of 1 / delta

    uint64_t m_velocity{1};                 //!< Current velocity, doubles per round
    CopaDirection_t m_direction{COPA_NONE}; //!< Current direction
    uint64_t m_numTimesDirectionSame{0};    //!< Consecutive rounds in same direction
    uint32_t m_lastRecordedCwnd{0};         //!< cwnd at last direction update
    Time m_lastCwndRecordTime{Seconds(0)};  //!< Time of last direction update

    bool m_slowStart{true};                //!< Copa's slow start is still running
    Time m_lastSrtt;                       //!< Last smoothed RTT seen (for velocity timing)

    bool m_isInitialized{false}; //!< True once the connection has been initialized
    Ptr<TcpRateOps> m_rateOps;   //!< Rate operations

    // Adaptive delta (wireless model, estimated online)
    bool m_adaptive{false};             //!< Choose delta online from the wireless model
    double m_adaptiveMin{0.001};         //!< Lower bound of delta
    double m_adaptiveMax{0.5};          //!< Upper bound of delta, delta of the low-load probes
    uint32_t m_halfRounds{8};           //!< Rounds per half-period (one measurement)
    uint32_t m_settleRounds{3};         //!< Rounds skipped after each change of delta
    uint32_t m_probeEvery{120};         //!< Half-periods per low-load probing cycle
    double m_weight{0.3};               //!< Weight of a new half-period in k
    double m_gain{0.25};                //!< Exponent of the delta update near the target
    double m_gainFar{0.25};              //!< Exponent of the delta update far from the target
    double m_farRatio{1.5};             //!< dq / dq* beyond which the far exponent is used
    double m_fairIncrease{10};           //!< Additive increase of 1/delta [packets], 0 = multiplicative
    double m_fairIncreaseFar{30};        //!< High-BDP far-below-target additive increase of 1/delta
    Time m_idleMargin{MilliSeconds(1)}; //!< Packet did not queue: RTT - RTTmin <= j + margin

    double m_jitter{-1};        //!< Jitter floor j [s] (-1: not yet measured)
    double m_ratio{0};          //!< k = c / mu [s] (0: not yet estimated)
    double m_rho{0};            //!< Last estimate of the total utilization
    double m_baseRtt{0};        //!< R0' [s]
    double m_dqTarget{0};       //!< Power-optimal queueing delay dq* [s] (0: not yet known)
    double m_deltaRun{0};       //!< Delta outside the probes (0: not yet set)
    Time m_lastAckTime;         //!< Time of the previous ACK
    double m_roundDqSum{0};     //!< Sum of dq over the ACKs of the round [s]
    uint32_t m_roundAcks{0};    //!< ACKs in the current round
    uint32_t m_roundIdle{0};    //!< ACKs of the round whose packet did not queue
    double m_roundMaxGap{0};    //!< Longest gap between ACKs in the round [s]
    uint64_t m_roundMinRttUs{0}; //!< Lowest packet RTT in the current round [us]
    Time m_roundStart;          //!< Start of the current round
    uint32_t m_rounds{0};       //!< Rounds since the end of the slow start
    double m_segDqSum{0};       //!< Sum of dq over the ACKs of the settled rounds [s]
    uint32_t m_segAcks{0};      //!< ACKs of the settled rounds
    uint32_t m_segIdle{0};      //!< Those whose packet did not queue
    double m_segMaxGap{0};      //!< Longest gap between ACKs in the half-period [s]
    uint64_t m_segMinRttUs{0};  //!< Lowest packet RTT in the settled rounds [us]
    bool m_segProbe{false};     //!< The half-period overlapped a time-aligned probe
    uint32_t m_floorShiftStreak{0}; //!< Consecutive settled half-periods with lifted RTT floor
    bool m_handoverDrainPending{false}; //!< Request short RefreshRttMin check for RTT step-up
    bool m_firstProbeDone{false}; //!< The queue left by the slow start has drained
    bool m_alignedProbeSeen{false}; //!< A time-aligned probe has given the jitter floor
    double m_lastProbeDq{0};    //!< Mean dq of the previous half-period of the first probe [s]
    Time m_probePeriod{Seconds(20)};   //!< Period of the time-aligned probes, 0 = round-based
    Time m_probeDuration{Seconds(2)}; //!< Length of a time-aligned probe
    double m_probeDqSum{0};     //!< Sum of dq in the measured half of the probe [s]
    uint32_t m_probeAcks{0};    //!< ACKs in the measured half of the probe

    // Adaptive probes, drains and fast recovery
    bool m_adaptiveProbes{true};          //!< Probe only while j is unsettled
    uint32_t m_stableProbes{3};           //!< Agreeing probes for a settled j
    Time m_stableTolAbs{MicroSeconds(500)}; //!< Agreement tolerance, absolute part
    double m_stableTolRel{0.1};           //!< Agreement tolerance, relative part
    Time m_reprobeRttAbs{MilliSeconds(5)}; //!< RTTmin change that restarts j, absolute part
    double m_reprobeRttRel{0.2};          //!< RTTmin change that restarts j, relative part
    Time m_fallbackPeriod{Seconds(120)};  //!< First fallback period once j is settled
    Time m_fallbackMax{Seconds(600)};     //!< Longest fallback period, always probed
    double m_recovery{0.5};               //!< Window restored after a legacy probe, 0 = none
    bool m_drainEnabled{true};            //!< Enable short drains in non-probing aligned slots
    Time m_drainMin{MilliSeconds(50)};    //!< Shortest drain, 0 disables drains
    double m_drainWindowRatio{0.5};       //!< Drain / gentle-probe window as a fraction of the BDP
    Time m_drainSkip{Seconds(10)};        //!< No drain if a packet found the queue idle since
    double m_jitterFloorMs{-1.0};         //!< Fixed jitter floor [ms] (< 0 probes j online)
    bool m_gentleProbe{true};             //!< Hold 0.5*BDP instead of jumping to AdaptiveDeltaMax
    uint32_t m_probeRtts{16};             //!< Probe length in smoothed RTTs, 0 = fixed
    Time m_probeDurationMin{MilliSeconds(300)}; //!< Shortest probe
    uint32_t m_boardGroup{0};             //!< Flows of a group share probe requests

    uint64_t m_decidedSlot{UINT64_MAX};   //!< Slot whose probe decision was taken
    uint64_t m_probingSlot{UINT64_MAX};   //!< Slot that probes
    double m_slotDuration{0};             //!< Probe length of the decided slot [s]
    uint32_t m_fallbackLevel{0};          //!< Fallback period doubles per agreeing probe
    uint32_t m_agreeing{0};               //!< Consecutive probes agreeing with j
    bool m_stable{false};                 //!< j is settled
    bool m_resetNext{false};              //!< The next probe replaces j
    double m_rttMinAtProbe{-1};           //!< RTTmin at the last probe [s]
    double m_probeHi{0};                  //!< Highest probe since the estimate (re)started [s]
    uint32_t m_settleHalves{0};           //!< Half-periods to discard after the first probe

    // 2-bucket windowed-max delivery bandwidth estimator
    Time m_bwPrevAck{Seconds(-1)};     //!< Last ACK time seen by the BW estimator
    Time m_bwWindowStart{Seconds(-1)}; //!< Start of the current BW sample window
    Time m_bwBucketStart{Seconds(-1)}; //!< Start of the current 2-bucket rotation window
    uint64_t m_bwBucketDelivered{0};   //!< Bytes delivered in the current sample window
    double m_bwCurMax{0};              //!< Current bucket peak delivery rate [bytes/s]
    double m_bwPrevMax{0};             //!< Previous bucket peak delivery rate [bytes/s]
    Time m_bwLastUpdate{Seconds(-1)};  //!< Last time the current bucket updated
    Time m_bwPrevUpdate{Seconds(-1)};  //!< Last time the previous bucket updated

    // Rate-based DrainController and legacy probe recovery
    DrainPhase m_drainPhase{PHASE_NORMAL};         //!< Drain/recovery phase
    DrainKind m_drainKind{DRAIN_REFRESH_RTT_MIN};  //!< Active drain purpose
    bool m_drainHasSlot{false};                    //!< Whether active drain is aligned to a slot
    uint64_t m_drainSlot{0};                       //!< Aligned slot of the active drain
    uint64_t m_evaluatedSlot{UINT64_MAX};          //!< Last aligned slot evaluated for drain
    Time m_drainStart{Seconds(0)};                 //!< Start time of active drain
    Time m_drainMaxEnd{Seconds(0)};                //!< Hard deadline of active drain
    bool m_drainHoldFull{false};                   //!< Peer extended slot: sample after full wait
    double m_drainGain{0.85};                      //!< Pacing factor vs estimated bandwidth
    double m_drainRate{0};                         //!< Locked pacing rate during drain [bytes/s]
    uint32_t m_drainSoftCwnd{0};                   //!< 0.90 * BDP smooth cwnd target during drain
    uint32_t m_savedCwnd{0};                       //!< cwnd when probe or drain began
    double m_savedRate{0};                         //!< Pacing rate when probe or drain began [bytes/s]
    uint64_t m_savedRttMinUs{0};                   //!< RTTmin when probe or drain began
    uint64_t m_trustedAtStartUs{0};                //!< Trusted baseline RTTmin at drain start
    uint64_t m_drainMinRttUs{0};                   //!< Lowest raw RTT sample observed during drain
    Time m_drainRoundStart{Seconds(0)};            //!< Start of current drain plateau round
    uint64_t m_drainRoundMinUs{0};                 //!< Minimum RTT in current drain round
    uint64_t m_drainPrevRoundMinUs{0};             //!< Minimum RTT of previous drain round
    uint32_t m_drainFlatRounds{0};                 //!< Consecutive non-dropping rounds in drain
    bool m_drainIdleSinceSet{false};               //!< Baseline or plateau reached during MeasureJitter
    Time m_drainIdleSince{Seconds(0)};             //!< Time baseline/plateau was detected
    double m_drainProbeDqSum{0};                   //!< Sum of dq samples while measuring j
    uint32_t m_drainProbeAcks{0};                  //!< Count of dq samples while measuring j
    bool m_drainCongested{false};                  //!< Loss cancelled drain recovery
    uint64_t m_drainBdpEst{0};                     //!< Estimated BDP at drain start [bytes]
    uint32_t m_drainBackoffSlots{1};               //!< 1 -> 2 -> 4 -> 8 slot backoff for RefreshRttMin
    uint64_t m_nextDrainSlot{0};                   //!< Earliest slot eligible for unrequested drain
    Time m_lastIdle{Seconds(-1)};                  //!< Last packet that found the queue idle
    uint64_t m_trustedMinUs{0};                    //!< RTTmin after last drain/probe (0: none)
    Time m_paceCapUntil{Seconds(0)};               //!< End of post-drain pacing cap
    double m_paceCap{0};                           //!< Post-drain pacing cap [bytes/s]
    bool m_lastAppLimited{false};                  //!< Latest rate sample was app-limited
    EventId m_drainTimer;                          //!< Timer for hard-deadline drain/recovery expiry
    Ptr<TcpSocketState> m_lastTcb;                 //!< Socket state for timer expiry outside ACKs

    bool m_probeSaved{false};                      //!< Legacy delta_max aligned probe is running
    bool m_savedCongested{false};                  //!< Legacy probe saw congestion

    /// Estimator state, once per round trip
    TracedCallback<double, double, double, double, double, double> m_adaptiveTrace;
};

} // namespace ns3

#endif // TCP_COPA_H
