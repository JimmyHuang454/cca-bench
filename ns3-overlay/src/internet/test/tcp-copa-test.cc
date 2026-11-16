/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Unit tests of TcpCopa against Section 2 of the paper (Arun and
 * Balakrishnan, "Copa: Practical Delay-Based Congestion Control for the
 * Internet", NSDI 2018). Each test drives CongControl() with synthetic RTT
 * samples at scheduled simulation times.
 */

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/tcp-copa.h"
#include "ns3/tcp-rate-ops.h"
#include "ns3/tcp-socket-state.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"

#include <functional>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpCopaTestSuite");

/**
 * @ingroup internet-test
 *
 * @brief Base of the Copa tests: a socket state, a Copa instance and a helper
 * that delivers one ACK with a given RTT sample.
 */
class TcpCopaTestBase : public TestCase
{
  public:
    /**
     * @brief Constructor.
     * @param name description of the test
     */
    TcpCopaTestBase(const std::string& name);

  protected:
    void DoRun() override;

    /**
     * @brief The test body, run at simulation time 0.
     */
    virtual void Body() = 0;

    /**
     * @brief Deliver one ACK to Copa.
     * @param rtt RTT sample of the ACK
     * @param ackedSegments number of segments acknowledged
     * @param lostSegments number of segments newly marked lost
     */
    void Ack(Time rtt, uint32_t ackedSegments = 1, uint32_t lostSegments = 0);

    /**
     * @brief Deliver one ACK per call of rttAt() every step until end.
     * @param start time of the first ACK, relative to now
     * @param end time of the last ACK, relative to now
     * @param step interval between ACKs
     * @param rttAt RTT sample as a function of the ACK time
     */
    void AckSeries(Time start, Time end, Time step, std::function<Time(Time)> rttAt);

    /**
     * @brief Run the simulator until the given absolute time.
     * @param t absolute simulation time
     */
    void RunUntil(Time t);

    Ptr<TcpSocketState> m_tcb; //!< Socket state
    Ptr<TcpCopa> m_copa;       //!< Algorithm under test
    uint32_t m_mss{1000};      //!< Segment size
};

TcpCopaTestBase::TcpCopaTestBase(const std::string& name)
    : TestCase(name)
{
}

void
TcpCopaTestBase::DoRun()
{
    m_tcb = CreateObject<TcpSocketState>();
    m_tcb->m_segmentSize = m_mss;
    m_tcb->m_cWnd = 10 * m_mss;
    m_tcb->m_maxPacingRate = DataRate("100Gbps");
    m_copa = CreateObject<TcpCopa>();
    m_copa->CongestionStateSet(m_tcb, TcpSocketState::CA_OPEN);
    Body();
    Simulator::Destroy();
}

void
TcpCopaTestBase::Ack(Time rtt, uint32_t ackedSegments, uint32_t lostSegments)
{
    m_tcb->m_lastRtt = rtt;
    // Standard EWMA of the RTT samples.
    if (m_tcb->m_srtt.Get().IsZero())
    {
        m_tcb->m_srtt = rtt;
    }
    else
    {
        m_tcb->m_srtt = m_tcb->m_srtt.Get() + (rtt - m_tcb->m_srtt.Get()) / 8;
    }
    TcpRateOps::TcpRateConnection rc;
    TcpRateOps::TcpRateSample rs;
    rs.m_ackedSacked = ackedSegments * m_mss;
    rs.m_bytesLoss = lostSegments * m_mss;
    m_copa->CongControl(m_tcb, rc, rs);
}

void
TcpCopaTestBase::AckSeries(Time start, Time end, Time step, std::function<Time(Time)> rttAt)
{
    for (Time t = start; t <= end; t += step)
    {
        Time abs = Simulator::Now() + t;
        Simulator::Schedule(t, [this, abs, rttAt]() { Ack(rttAt(abs)); });
    }
}

void
TcpCopaTestBase::RunUntil(Time t)
{
    Simulator::Stop(t - Simulator::Now());
    Simulator::Run();
}

/**
 * @ingroup internet-test
 *
 * @brief Pacing rate is 2 * cwnd / RTTstanding (Section 2.1).
 */
class TcpCopaPacingTest : public TcpCopaTestBase
{
  public:
    TcpCopaPacingTest()
        : TcpCopaTestBase("Copa paces at 2 * cwnd / RTTstanding")
    {
    }

  private:
    void Body() override
    {
        // RTTstanding is the minimum over srtt/2: 100 ms, although the last
        // sample is 120 ms.
        AckSeries(MilliSeconds(0), MilliSeconds(40), MilliSeconds(10), [](Time) {
            return MilliSeconds(100);
        });
        Simulator::Schedule(MilliSeconds(45), [this]() { Ack(MilliSeconds(120)); });
        RunUntil(MilliSeconds(50));
        NS_TEST_ASSERT_MSG_EQ(m_tcb->m_pacing, true, "Copa must enable pacing");
        const double expected = 2.0 * 8 * m_tcb->m_cWnd.Get() / 0.100;
        NS_TEST_ASSERT_MSG_EQ_TOL(m_tcb->m_pacingRate.Get().GetBitRate(),
                                  expected,
                                  expected * 1e-6,
                                  "pacing rate is not 2 * cwnd / RTTstanding");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief The queueing delay is RTTstanding - RTTmin (Eq. 2), not the latest
 * sample: a single delayed ACK (ACK compression, jitter) must not make Copa
 * reduce its window.
 */
class TcpCopaStandingRttTest : public TcpCopaTestBase
{
  public:
    TcpCopaStandingRttTest()
        : TcpCopaTestBase("Copa queueing delay uses RTTstanding")
    {
    }

  private:
    void Body() override
    {
        // Leave slow start: a standing queue of 50 ms with cwnd = 10 packets
        // over 150 ms is above the target rate 1 / (0.5 * 0.05 s) = 40 pkt/s.
        AckSeries(MilliSeconds(0), MilliSeconds(200), MilliSeconds(10), [](Time t) {
            return t < MilliSeconds(100) ? MilliSeconds(100) : MilliSeconds(150);
        });
        RunUntil(MilliSeconds(300));
        // Queue drained: RTT back to the minimum, then one late ACK.
        AckSeries(MilliSeconds(0), MilliSeconds(200), MilliSeconds(10), [](Time) {
            return MilliSeconds(100);
        });
        RunUntil(MilliSeconds(510));
        const uint32_t before = m_tcb->m_cWnd.Get();
        Ack(MilliSeconds(400));
        NS_TEST_ASSERT_MSG_GT(m_tcb->m_cWnd.Get(),
                              before,
                              "a single late ACK must not reduce cwnd (dq = RTTstanding - RTTmin)");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief Velocity: doubles once per window after the direction stayed the same
 * for three windows, and resets to 1 when the direction changes (Section 2.1).
 */
class TcpCopaVelocityTest : public TcpCopaTestBase
{
  public:
    TcpCopaVelocityTest()
        : TcpCopaTestBase("Copa velocity doubles after three windows and resets")
    {
    }

  private:
    void Body() override
    {
        // Leave slow start with a short burst of queueing.
        AckSeries(MilliSeconds(0), MilliSeconds(200), MilliSeconds(10), [](Time t) {
            return t < MilliSeconds(100) ? MilliSeconds(100) : MilliSeconds(200);
        });
        RunUntil(MilliSeconds(300));
        // Empty queue for many RTTs: cwnd keeps increasing.
        AckSeries(MilliSeconds(0), MilliSeconds(1500), MilliSeconds(5), [](Time) {
            return MilliSeconds(100);
        });
        RunUntil(MilliSeconds(1805));
        NS_TEST_ASSERT_MSG_EQ(m_copa->GetDirection(), 1, "direction must be up");
        NS_TEST_ASSERT_MSG_GT(m_copa->GetVelocity(), 2, "velocity must have doubled");
        // A long standing queue: the direction turns down and the velocity resets.
        AckSeries(MilliSeconds(0), MilliSeconds(1000), MilliSeconds(5), [](Time) {
            return MilliSeconds(400);
        });
        RunUntil(MilliSeconds(2810));
        NS_TEST_ASSERT_MSG_EQ(m_copa->GetDirection(), 2, "direction must be down");
        NS_TEST_ASSERT_MSG_LT(m_copa->GetVelocity(), 4, "velocity must have been reset");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief Mode switching (Section 2.2): in the default mode losses are
 * ignored; a queue that is never nearly empty for five RTTs selects the
 * competitive mode, where 1/delta increases additively and halves on loss;
 * a nearly empty queue brings back the default mode with delta = 0.5.
 */
class TcpCopaModeTest : public TcpCopaTestBase
{
  public:
    TcpCopaModeTest()
        : TcpCopaTestBase("Copa switches between default and competitive mode")
    {
    }

  private:
    void Body() override
    {
        // Default mode, empty queue: a loss changes nothing.
        AckSeries(MilliSeconds(0), MilliSeconds(300), MilliSeconds(10), [](Time) {
            return MilliSeconds(100);
        });
        RunUntil(MilliSeconds(310));
        const uint32_t cwnd = m_tcb->m_cWnd.Get();
        Ack(MilliSeconds(100), 1, 1);
        NS_TEST_ASSERT_MSG_EQ(m_copa->IsCompetitive(), false, "must start in default mode");
        NS_TEST_ASSERT_MSG_EQ_TOL(m_copa->GetDelta(), 0.5, 1e-9, "default delta is 0.5");
        NS_TEST_ASSERT_MSG_GT_OR_EQ(m_tcb->m_cWnd.Get(), cwnd, "default mode ignores loss");

        // A buffer-filling competitor: the RTT oscillates between 300 and 340 ms
        // and never comes back near the 100 ms minimum.
        AckSeries(MilliSeconds(0), MilliSeconds(4000), MilliSeconds(5), [](Time t) {
            return MilliSeconds(300 + 40 * ((t.GetMilliSeconds() / 50) % 2));
        });
        RunUntil(MilliSeconds(4320));
        NS_TEST_ASSERT_MSG_EQ(m_copa->IsCompetitive(), true, "must switch to competitive mode");
        const double invDelta = 1.0 / m_copa->GetDelta();
        NS_TEST_ASSERT_MSG_GT(invDelta, 2.0, "1/delta must increase in competitive mode");

        Ack(MilliSeconds(320), 1, 1);
        NS_TEST_ASSERT_MSG_EQ_TOL(1.0 / m_copa->GetDelta(),
                                  std::max(2.0, invDelta / 2),
                                  0.05,
                                  "a loss must halve 1/delta");
        // A second loss within the same RTT is ignored.
        const double afterLoss = 1.0 / m_copa->GetDelta();
        Ack(MilliSeconds(320), 1, 1);
        NS_TEST_ASSERT_MSG_EQ_TOL(1.0 / m_copa->GetDelta(),
                                  afterLoss,
                                  0.05,
                                  "1/delta is halved at most once per RTT");

        // The competitor leaves: the queue empties and the default mode returns.
        AckSeries(MilliSeconds(0), MilliSeconds(300), MilliSeconds(5), [](Time) {
            return MilliSeconds(100);
        });
        RunUntil(MilliSeconds(4700));
        NS_TEST_ASSERT_MSG_EQ(m_copa->IsCompetitive(), false, "must return to default mode");
        NS_TEST_ASSERT_MSG_EQ_TOL(m_copa->GetDelta(), 0.5, 1e-9, "default mode resets delta");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief After a retransmission timeout (cwnd = 1 segment) Copa restarts its
 * slow start and doubles cwnd once per RTT while below the target rate.
 */
class TcpCopaRtoTest : public TcpCopaTestBase
{
  public:
    TcpCopaRtoTest()
        : TcpCopaTestBase("Copa restarts slow start after a timeout")
    {
    }

  private:
    void Body() override
    {
        // Leave slow start.
        AckSeries(MilliSeconds(0), MilliSeconds(200), MilliSeconds(10), [](Time t) {
            return t < MilliSeconds(100) ? MilliSeconds(100) : MilliSeconds(200);
        });
        RunUntil(MilliSeconds(300));
        m_copa->CongestionStateSet(m_tcb, TcpSocketState::CA_LOSS);
        m_tcb->m_cWnd = m_mss;
        AckSeries(MilliSeconds(0), MilliSeconds(700), MilliSeconds(10), [](Time) {
            return MilliSeconds(100);
        });
        RunUntil(MilliSeconds(1010));
        // About six doublings in 700 ms at a 100 ms RTT.
        NS_TEST_ASSERT_MSG_GT_OR_EQ(m_tcb->m_cWnd.Get(),
                                    16 * m_mss,
                                    "cwnd must grow exponentially after a timeout");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief copa-auto with adaptive probes: the flow probes in the next aligned slot while its
 * jitter floor is unsettled, and the probe lasts AdaptiveProbeRtts smoothed RTTs rather than the
 * whole AdaptiveProbeDuration.
 */
class TcpCopaAdaptiveProbeTest : public TcpCopaTestBase
{
  public:
    TcpCopaAdaptiveProbeTest()
        : TcpCopaTestBase("copa-auto probes for AdaptiveProbeRtts round trips")
    {
    }

  private:
    void Body() override
    {
        m_copa->SetAttribute("AdaptiveDelta", BooleanValue(true));
        m_copa->SetAttribute("ModeSwitching", BooleanValue(false));
        m_copa->SetAttribute("AdaptiveGentleProbe", BooleanValue(false));
        // A bottleneck of 1000 packets/s behind a 50 ms base RTT, one ACK per millisecond: the
        // RTT grows by 1 ms per packet of cwnd above the 50-packet BDP
        for (Time t = MilliSeconds(0); t < Seconds(22); t += MilliSeconds(1))
        {
            Simulator::Schedule(t, [this]() {
                const double excess = m_tcb->m_cWnd.Get() / double(m_mss) - 50;
                Ack(MilliSeconds(50) + MicroSeconds(static_cast<int64_t>(1000 * std::max(0.0, excess))));
            });
        }
        RunUntil(Seconds(20.3));
        NS_TEST_ASSERT_MSG_EQ_TOL(m_copa->GetDelta(),
                                  0.5,
                                  1e-12,
                                  "the second aligned slot must probe at AdaptiveDeltaMax");
        RunUntil(Seconds(21.8));
        NS_TEST_ASSERT_MSG_LT(m_copa->GetDelta(),
                              0.5,
                              "a probe of 16 RTTs (about 1 s) must be over 1.8 s into the slot");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief Scheme 1 gentle aligned probe: holds 0.5 * BDP instead of jumping to
 * AdaptiveDeltaMax, does not collapse under jitter or falsely enter competitive mode,
 * and restores the pre-probe cwnd in full at probe exit.
 */
class TcpCopaGentleProbeTest : public TcpCopaTestBase
{
  public:
    TcpCopaGentleProbeTest()
        : TcpCopaTestBase(
              "copa-auto gentle probe regulates rate, ramps cwnd, and restores pre-probe cwnd")
    {
    }

  private:
    void Body() override
    {
        m_copa->SetAttribute("AdaptiveDelta", BooleanValue(true));
        m_copa->SetAttribute("ModeSwitching", BooleanValue(true));
        m_copa->SetAttribute("AdaptiveProbeRtts", UintegerValue(0));
        // Settle past slow start and initial post-slow-start calibration; periodic 80 ms peaks
        // keep 0.1 * (RTTmax - RTTmin) = 8 ms above steady dq so ModeSwitching stays in default
        // mode while 10 ms queueing bursts adapt delta below 0.5.
        for (Time t = MilliSeconds(0); t <= Seconds(19.99); t += MilliSeconds(2))
        {
            Simulator::Schedule(t, [this, t]() {
                const double excess = m_tcb->m_cWnd.Get() / double(m_mss) - 50;
                const int64_t ph = t.GetMilliSeconds() % 160;
                const uint64_t burstUs = (ph >= 145) ? 80000 : ((ph >= 35) ? 10000 : 0);
                Ack(MilliSeconds(50) +
                    MicroSeconds(burstUs + static_cast<int64_t>(250 * std::max(0.0, excess))));
            });
        }
        RunUntil(Seconds(19.995));
        const uint32_t preProbeCwnd = m_tcb->m_cWnd.Get();
        const DataRate preProbePacing = m_tcb->m_pacingRate.Get();
        NS_TEST_ASSERT_MSG_EQ(m_copa->IsCompetitive(),
                              false,
                              "flow must stay in default mode before probe");
        NS_TEST_ASSERT_MSG_LT(m_copa->GetDelta(), 0.5, "adaptive delta must settle below 0.5");

        for (Time t = Seconds(20.02); t < Seconds(20.65); t += MilliSeconds(5))
        {
            Simulator::Schedule(t - Simulator::Now(), [this]() { Ack(MilliSeconds(80)); });
        }
        RunUntil(Seconds(20.15));
        NS_TEST_ASSERT_MSG_LT(m_copa->GetDelta(), 0.5, "gentle probe must not jump delta");
        NS_TEST_ASSERT_MSG_EQ(m_copa->IsCompetitive(),
                              false,
                              "gentle drain must not trigger competitive mode");
        NS_TEST_ASSERT_MSG_GT_OR_EQ(m_tcb->m_cWnd.Get(),
                                    10 * m_mss,
                                    "gentle drain keeps cwnd above floor without collapsing");
        NS_TEST_ASSERT_MSG_LT(m_tcb->m_cWnd.Get(),
                              preProbeCwnd,
                              "gentle drain ramps cwnd toward 0.90*BDP under inflated RTT");
        NS_TEST_ASSERT_MSG_LT(m_tcb->m_pacingRate.Get().GetBitRate(),
                              preProbePacing.GetBitRate(),
                              "gentle drain pacing rate (0.78*bw_est) must drop below pre-probe rate");

        // Send low-RTT ACKs after the plateau/drain period and verify early exit restores cwnd
        // before the full 2.0 s aligned slot elapses.
        RunUntil(Seconds(20.75));
        Ack(MilliSeconds(50));
        NS_TEST_ASSERT_MSG_GT_OR_EQ(m_tcb->m_cWnd.Get(),
                                    preProbeCwnd - m_mss,
                                    "gentle probe early exit must restore pre-probe cwnd");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief Fixed AdaptiveJitterFloorMs skips jitter-floor probing while respecting AdaptiveDrain.
 */
class TcpCopaFixedJitterTest : public TcpCopaTestBase
{
  public:
    TcpCopaFixedJitterTest()
        : TcpCopaTestBase("copa-auto fixed jitter floor skips probe and obeys AdaptiveDrain")
    {
    }

  private:
    void Body() override
    {
        m_copa->SetAttribute("AdaptiveDelta", BooleanValue(true));
        m_copa->SetAttribute("ModeSwitching", BooleanValue(false));
        m_copa->SetAttribute("AdaptiveJitterFloorMs", DoubleValue(2.0));
        m_copa->SetAttribute("AdaptiveDrain", BooleanValue(true));
        m_copa->SetAttribute("AdaptiveDrainSkip", TimeValue(Seconds(0)));
        m_copa->SetAttribute("AdaptiveProbeRtts", UintegerValue(0));
        for (Time t = MilliSeconds(0); t < Seconds(19.99); t += MilliSeconds(2))
        {
            Simulator::Schedule(t, [this]() {
                const double excess = m_tcb->m_cWnd.Get() / double(m_mss) - 50;
                Ack(MilliSeconds(50) +
                    MicroSeconds(static_cast<int64_t>(1000 * std::max(0.0, excess))));
            });
        }
        RunUntil(Seconds(19.995));
        const uint32_t preDrainCwnd = m_tcb->m_cWnd.Get();
        const DataRate preDrainPacing = m_tcb->m_pacingRate.Get();

        for (Time t = Seconds(20.005); t < Seconds(20.05); t += MilliSeconds(2))
        {
            Simulator::Schedule(t - Simulator::Now(), [this]() { Ack(MilliSeconds(75)); });
        }
        RunUntil(Seconds(20.045));
        NS_TEST_ASSERT_MSG_LT(m_copa->GetDelta(), 0.5, "fixed j skips delta_max probe");
        NS_TEST_ASSERT_MSG_LT(m_tcb->m_pacingRate.Get().GetBitRate(),
                              preDrainPacing.GetBitRate(),
                              "non-probing slot must enter rate-based drain when AdaptiveDrain=true");
        NS_TEST_ASSERT_MSG_LT_OR_EQ(m_tcb->m_cWnd.Get(),
                                    preDrainCwnd,
                                    "rate-based drain ramps cwnd gently without growing");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief copa-auto starting from an aggressive initial delta (0.05) must not falsely enter
 * competitive mode before its first adaptive estimate is ready, and competitive mode uses
 * AdaptiveDeltaMax (0.5) as its ceiling/reset delta rather than the initial LatencyFactor.
 */
class TcpCopaAdaptiveModeCouplingTest : public TcpCopaTestBase
{
  public:
    TcpCopaAdaptiveModeCouplingTest()
        : TcpCopaTestBase("copa-auto decouples initial delta from competitive mode")
    {
    }

  private:
    void Body() override
    {
        m_copa->SetAttribute("LatencyFactor", DoubleValue(0.05));
        m_copa->SetAttribute("AdaptiveDelta", BooleanValue(true));
        m_copa->SetAttribute("ModeSwitching", BooleanValue(true));
        m_copa->SetAttribute("AdaptiveJitterFloorMs", DoubleValue(0.0));
        // Startup queue while m_slowStart / m_deltaRun == 0 must not flip to competitive mode.
        Ack(MilliSeconds(50));
        AckSeries(MilliSeconds(10), MilliSeconds(600), MilliSeconds(5), [](Time) {
            return MilliSeconds(65);
        });
        RunUntil(Seconds(0.65));
        NS_TEST_ASSERT_MSG_EQ(m_copa->IsCompetitive(),
                              false,
                              "must not enter competitive mode before first adaptive estimate");

        // Run with periodic 60 ms idle gaps (> srtt/2) so slow start exits and m_deltaRun updates
        // (> 0) while ModeSwitching sees nearly-empty queues and stays in default mode.
        for (Time t = Seconds(0.7); t < Seconds(6.0); t += MilliSeconds(2))
        {
            Simulator::Schedule(t - Simulator::Now(), [this, t]() {
                const double excess = m_tcb->m_cWnd.Get() / double(m_mss) - 50;
                const int64_t ph = t.GetMilliSeconds() % 240;
                if (ph < 60)
                {
                    Ack(MilliSeconds(50));
                }
                else
                {
                    Ack(MilliSeconds(60) +
                        MicroSeconds(static_cast<int64_t>(1000 * std::max(0.0, excess))));
                }
            });
        }
        RunUntil(Seconds(6.05));
        NS_TEST_ASSERT_MSG_EQ(m_copa->IsCompetitive(), false, "single flow must stay in default mode");
        NS_TEST_ASSERT_MSG_GT(std::abs(m_copa->GetDelta() - 0.05),
                              1e-4,
                              "adaptive delta must update before competitor test");

        // A buffer-filling competitor arrives: queue stays non-empty for > 5*srtt and triggers
        // competitive mode, starting at AdaptiveDeltaMax (0.5) rather than 0.05.
        AckSeries(MilliSeconds(0), Seconds(4), MilliSeconds(5), [](Time t) {
            return MilliSeconds(300 + 40 * ((t.GetMilliSeconds() / 50) % 2));
        });
        RunUntil(Seconds(10.1));
        NS_TEST_ASSERT_MSG_EQ(m_copa->IsCompetitive(), true, "must enter competitive mode on persistent queue");
        const double invDelta = 1.0 / m_copa->GetDelta();
        NS_TEST_ASSERT_MSG_GT(invDelta, 2.0, "1/delta must grow from 2.0 in competitive mode");
        // Halve 1/delta on repeated losses: ceiling must be AdaptiveDeltaMax (0.5), not 0.05.
        for (int i = 0; i < 6; ++i)
        {
            Ack(MilliSeconds(320), 1, 1);
            RunUntil(Simulator::Now() + MilliSeconds(350));
        }
        NS_TEST_ASSERT_MSG_EQ_TOL(m_copa->GetDelta(),
                                  0.5,
                                  1e-6,
                                  "competitive loss ceiling must stay AdaptiveDeltaMax (0.5)");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief RefreshRttMin drain exits early as soon as standing RTT returns to trusted baseline.
 */
class TcpCopaRateDrainBaselineExitTest : public TcpCopaTestBase
{
  public:
    TcpCopaRateDrainBaselineExitTest()
        : TcpCopaTestBase("copa-auto RefreshRttMin exits early on baseline return")
    {
    }

  private:
    void Body() override
    {
        m_copa->SetAttribute("AdaptiveDelta", BooleanValue(true));
        m_copa->SetAttribute("ModeSwitching", BooleanValue(false));
        m_copa->SetAttribute("AdaptiveJitterFloorMs", DoubleValue(1.0));
        m_copa->SetAttribute("AdaptiveDrain", BooleanValue(true));
        m_copa->SetAttribute("AdaptiveDrainSkip", TimeValue(Seconds(0)));
        m_copa->SetAttribute("AdaptiveProbeRtts", UintegerValue(0));

        // Establish 50 ms baseline and BW estimate before slot 1 (20 s). Keep a 50 ms sample
        // inside the 10 s RTTmin window so queue inflation raises standing RTT without drifting
        // RTTmin above trustedMin.
        for (Time t = MilliSeconds(0); t <= Seconds(10.5); t += MilliSeconds(2))
        {
            Simulator::Schedule(t, [this]() { Ack(MilliSeconds(50)); });
        }
        // Raise queue (standing RTT = 95 ms) up to slot 1.
        for (Time t = MilliSeconds(10502); t < Seconds(19.99); t += MilliSeconds(2))
        {
            Simulator::Schedule(t, [this]() { Ack(MilliSeconds(95)); });
        }
        RunUntil(Seconds(19.995));
        const uint32_t preDrainCwnd = m_tcb->m_cWnd.Get();
        const DataRate normalPacing = m_tcb->m_pacingRate.Get();

        // Enter slot 1 at t=20.005: high queue first, then queue drains to 50 ms after 80 ms.
        for (Time t = Seconds(20.005); t <= Seconds(20.07); t += MilliSeconds(5))
        {
            Simulator::Schedule(t - Simulator::Now(), [this]() { Ack(MilliSeconds(95)); });
        }
        RunUntil(Seconds(20.075));
        NS_TEST_ASSERT_MSG_LT(m_tcb->m_pacingRate.Get().GetBitRate(),
                              normalPacing.GetBitRate(),
                              "RefreshRttMin drain must pace below normal rate");

        for (Time t = Seconds(20.08); t <= Seconds(20.16); t += MilliSeconds(5))
        {
            Simulator::Schedule(t - Simulator::Now(), [this]() { Ack(MilliSeconds(50)); });
        }
        RunUntil(Seconds(20.17));
        // Should have exited early on baseline return (well before hard 1s cap) and restored cwnd.
        NS_TEST_ASSERT_MSG_GT_OR_EQ(m_tcb->m_cWnd.Get(),
                                    preDrainCwnd / 2,
                                    "early drain exit must restore window after queue clears");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief Non-persistent packet drops in copa-auto-NC must not cancel post-drain recovery, while
 * hard RTO (CA_LOSS) does abort recovery.
 */
class TcpCopaAutoNcLossTest : public TcpCopaTestBase
{
  public:
    TcpCopaAutoNcLossTest()
        : TcpCopaTestBase("copa-auto-NC ignores non-persistent loss during drain")
    {
    }

  private:
    void Body() override
    {
        m_copa->SetAttribute("AdaptiveDelta", BooleanValue(true));
        m_copa->SetAttribute("ModeSwitching", BooleanValue(false));
        m_copa->SetAttribute("AdaptiveJitterFloorMs", DoubleValue(1.0));
        m_copa->SetAttribute("AdaptiveDrain", BooleanValue(true));
        m_copa->SetAttribute("AdaptiveDrainSkip", TimeValue(Seconds(0)));
        m_copa->SetAttribute("AdaptiveProbeRtts", UintegerValue(0));

        for (Time t = MilliSeconds(0); t <= Seconds(19.99); t += MilliSeconds(2))
        {
            Simulator::Schedule(t, [this]() { Ack(MilliSeconds(50)); });
        }
        RunUntil(Seconds(19.995));
        const uint32_t preDrainCwnd = m_tcb->m_cWnd.Get();

        // Slot 1 drain with random packet loss signals in the middle (non-persistent).
        for (Time t = Seconds(20.005); t <= Seconds(20.25); t += MilliSeconds(5))
        {
            const uint32_t lost = (t == Seconds(20.03) || t == Seconds(20.06)) ? 1 : 0;
            Simulator::Schedule(t - Simulator::Now(),
                                [this, lost]() { Ack(MilliSeconds(50), 1, lost); });
        }
        RunUntil(Seconds(20.26));
        NS_TEST_ASSERT_MSG_GT_OR_EQ(m_tcb->m_cWnd.Get(),
                                    preDrainCwnd - m_mss,
                                    "copa-auto-NC must restore cwnd despite random loss");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief Hard-deadline drain timer fires even if ACKs stop arriving during drain.
 */
class TcpCopaDrainTimerExpiryTest : public TcpCopaTestBase
{
  public:
    TcpCopaDrainTimerExpiryTest()
        : TcpCopaTestBase("copa-auto drain timer expires cleanly without incoming ACKs")
    {
    }

  private:
    void Body() override
    {
        m_copa->SetAttribute("AdaptiveDelta", BooleanValue(true));
        m_copa->SetAttribute("ModeSwitching", BooleanValue(false));
        m_copa->SetAttribute("AdaptiveJitterFloorMs", DoubleValue(1.0));
        m_copa->SetAttribute("AdaptiveDrain", BooleanValue(true));
        m_copa->SetAttribute("AdaptiveDrainSkip", TimeValue(Seconds(0)));
        m_copa->SetAttribute("AdaptiveProbeRtts", UintegerValue(0));

        for (Time t = MilliSeconds(0); t <= Seconds(19.99); t += MilliSeconds(2))
        {
            Simulator::Schedule(t, [this]() { Ack(MilliSeconds(50)); });
        }
        RunUntil(Seconds(19.995));
        const uint32_t preDrainCwnd = m_tcb->m_cWnd.Get();

        // Trigger drain at slot 1 and stop ACKs; timer should restore cwnd by 21.1 s.
        Ack(MilliSeconds(80));
        RunUntil(Seconds(21.2));
        NS_TEST_ASSERT_MSG_GT_OR_EQ(m_tcb->m_cWnd.Get(),
                                    preDrainCwnd - m_mss,
                                    "drain timer expiry must restore cwnd when ACKs stall");
    }
};

/**
 * @ingroup internet-test
 *
 * @brief TestSuite for Copa.
 */
class TcpCopaTestSuite : public TestSuite
{
  public:
    TcpCopaTestSuite()
        : TestSuite("tcp-copa-test", Type::UNIT)
    {
        AddTestCase(new TcpCopaPacingTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaStandingRttTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaVelocityTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaModeTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaRtoTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaAdaptiveProbeTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaGentleProbeTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaFixedJitterTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaAdaptiveModeCouplingTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaRateDrainBaselineExitTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaAutoNcLossTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpCopaDrainTimerExpiryTest(), TestCase::Duration::QUICK);
    }
};

static TcpCopaTestSuite g_tcpCopaTest; //!< Static variable for test initialization

} // namespace ns3
