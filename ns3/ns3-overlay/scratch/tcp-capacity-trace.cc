/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Capacity-trace-driven bottleneck for evaluating congestion control under
 * mobility (scenario E).
 *
 * The bottleneck capacity follows a schedule that is known to the experiment
 * (ground truth) but never exposed to the senders. This makes recovery time,
 * utilisation and fairness exactly measurable, independently of any radio
 * model. Built-in profiles cover the typical disruptions of mobile networks:
 *
 *   outage     constant capacity with one outage (tunnel, elevator, garage)
 *   step       capacity drops by --stepFactor and later comes back
 *              (congested cell / back to an idle cell)
 *   handover   short outages (--hoGap) at random intervals around
 *              --hoInterval, each followed by a new random cell capacity in
 *              [--capMin, --capMax] (high-speed rail, highway driving)
 *   fluctuate  capacity random walk every --fluctPeriod seconds within
 *              [--capMin, --capMax] (urban driving, fast fading)
 *   mahimahi   packet-delivery trace replayed exactly (--downTrace, --upTrace,
 *              --traceOffset): each line is a millisecond at which 1500 bytes
 *              can leave, the trace wraps at its last timestamp, a packet may
 *              span several opportunities and an unused opportunity is lost.
 *              --queueBytes is the buffer in front of the gate. With
 *              --delayTrace each direction also gets the time-varying one-way
 *              delay of LeoReplayer (one value [ms] per 10 ms, looped, taken
 *              when a packet arrives). Unlike LeoReplayer, packets stay in order.
 *   file       schedule read from --traceFile, lines
 *              "time_s rate_mbps [loss [delay_ms]]" where loss = 1 means an
 *              outage and delay_ms (optional) is the bottleneck one-way
 *              propagation delay from then on (field traces, see
 *              fieldmeter-trace.py). A delay decrease is applied gradually
 *              (--delaySlew) so that packets in flight are not reordered.
 *
 * During an outage every packet is dropped in both directions (data and ACK),
 * like a coverage hole (--disruption=drop, default). With --disruption=hold
 * the packets are buffered instead, as a base station does during a handover
 * with data forwarding or an RLC AM re-establishment: nothing is delivered
 * during the outage, and afterwards the buffered data (up to --holdBytes) and
 * ACKs are delivered in order at the new capacity. Event times are jittered by +/- --eventJitter seconds
 * (drawn from the RNG run) so that replications do not always hit the same
 * phase of the congestion control probing cycles.
 *
 * Topology (one sender node per flow, so each flow can use its own TCP
 * variant):
 *
 *   sender_i --(1 Gbps, accessDelay)--> router ==(capacity trace)==> receiver
 *
 * The bottleneck queue is a FIFO of --queueBytes (deep per-user buffers of
 * cellular base stations) unless --aqm=fqcodel is given.
 *
 * Output files (prefix given by --prefix):
 *   -timeseries.csv  per --binMs bin: time, capacity, outage flag, per-flow
 *                    Mbps, smoothed RTT (rtt<i>_ms), cwnd of flow 0, the
 *                    mean RTT sample of the bin (lrtt<i>_ms) and the base
 *                    (propagation) RTT of the path (base_rtt_ms)
 *   -events.csv      time, event (outage-begin/end, step-down/up, cell-change)
 *   -metrics.json    configuration and per-flow summary metrics
 *
 * Example:
 *   ./ns3 run "tcp-capacity-trace --ccs=bbrv2 --profile=outage --outageDuration=3"
 *   ./ns3 run "tcp-capacity-trace --ccs=bbrv2,cubic --profile=handover --run=4"
 *
 * Algorithms: cubic, bbr, bbrv2, bbrv3, copa, newreno, bbrv2l / bbrv3l
 * (BBRv2/v3 in the Linux-faithful mode, see TcpBbrV2::LinuxLossResponse).
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TcpCapacityTrace");

namespace
{

/** One piece of the capacity schedule. */
struct Segment
{
    double start;      ///< Start time [s]
    double rateMbps;   ///< Bottleneck capacity [Mbit/s]
    double loss;       ///< Random data loss probability, 1 = outage
    std::string event; ///< Event name written when the segment starts
    double delayMs{-1.0}; ///< Bottleneck one-way delay [ms], < 0 = unchanged
};

/**
 * Receive-side gate of a bottleneck device: random loss in normal operation;
 * during an outage either drops everything or holds packets and releases them
 * in order at the link rate when the outage ends.
 */
class HoldGate : public ErrorModel
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::ScenarioEHoldGate").SetParent<ErrorModel>();
        return tid;
    }

    /**
     * Configure the gate.
     * @param dev device whose receive path is gated
     * @param inner random-loss model used outside outages
     * @param limit hold buffer [bytes]
     */
    void Setup(Ptr<PointToPointNetDevice> dev, Ptr<RateErrorModel> inner, uint64_t limit)
    {
        m_dev = dev;
        m_inner = inner;
        m_limit = limit;
    }

    /**
     * Start an outage.
     * @param hold true to buffer packets, false to drop them
     */
    void StartOutage(bool hold)
    {
        m_outage = true;
        m_hold = hold;
    }

    /**
     * End an outage and release the held packets at the given rate.
     * @param rate release rate
     */
    void EndOutage(DataRate rate)
    {
        m_outage = false;
        m_rate = rate;
        if (!m_buffer.empty() && !m_releasing)
        {
            m_releasing = true;
            // Outages are driven by global events: deliver in the node's context.
            Simulator::ScheduleWithContext(m_dev->GetNode()->GetId(),
                                           Seconds(0),
                                           &HoldGate::ReleaseOne,
                                           this);
        }
    }

    /** @return bytes dropped because the hold buffer was full */
    uint64_t GetOverflowBytes() const
    {
        return m_overflow;
    }

  private:
    bool DoCorrupt(Ptr<Packet> p) override
    {
        if (m_reinject)
        {
            return false;
        }
        if (m_outage && !m_hold)
        {
            return true;
        }
        if ((m_outage && m_hold) || m_releasing)
        {
            // Keep the order: while held packets drain, new ones queue behind.
            if (m_bytes + p->GetSize() <= m_limit)
            {
                m_buffer.push_back(p->Copy());
                m_bytes += p->GetSize();
            }
            else
            {
                m_overflow += p->GetSize();
            }
            return true;
        }
        return m_inner->IsCorrupt(p);
    }

    void DoReset() override
    {
    }

    void ReleaseOne()
    {
        if (m_outage || m_buffer.empty())
        {
            m_releasing = !m_buffer.empty() && !m_outage ? m_releasing : false;
            return;
        }
        Ptr<Packet> p = m_buffer.front();
        m_buffer.pop_front();
        m_bytes -= p->GetSize();
        const Time gap = m_rate.CalculateBytesTxTime(p->GetSize());
        m_reinject = true;
        m_dev->Receive(p);
        m_reinject = false;
        if (m_buffer.empty())
        {
            m_releasing = false;
            return;
        }
        Simulator::Schedule(gap, &HoldGate::ReleaseOne, this);
    }

    Ptr<PointToPointNetDevice> m_dev;   ///< Gated device
    Ptr<RateErrorModel> m_inner;        ///< Random loss outside outages
    std::deque<Ptr<Packet>> m_buffer;   ///< Held packets
    uint64_t m_bytes{0};                ///< Held bytes
    uint64_t m_limit{0};                ///< Hold buffer limit
    uint64_t m_overflow{0};             ///< Bytes dropped on a full hold buffer
    DataRate m_rate{"1Mbps"};           ///< Release rate
    bool m_outage{false};               ///< Outage in progress
    bool m_hold{false};                 ///< Hold (true) or drop (false) during outages
    bool m_releasing{false};            ///< Draining the held packets
    bool m_reinject{false};             ///< Delivering a held packet
};

/**
 * Receive-side gate that releases packets only at the delivery opportunities
 * of a Mahimahi trace (same semantics as Mahimahi's LinkQueue). The device
 * itself runs much faster than the trace, so the buffer of this gate is the
 * bottleneck buffer.
 */
class MahimahiGate : public ErrorModel
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::MahimahiGate").SetParent<ErrorModel>();
        return tid;
    }

    /**
     * Load a trace and schedule the first opportunity.
     * @param dev device whose receive path is gated
     * @param file trace file
     * @param limit buffer [bytes]
     * @param offsetMs position of the trace at simulation time 0 [ms]
     * @param delays one-way delay per 10 ms [ms], may be empty
     */
    void Setup(Ptr<PointToPointNetDevice> dev,
               const std::string& file,
               uint64_t limit,
               int64_t offsetMs,
               const std::vector<int64_t>& delays,
               double loss)
    {
        m_delays = delays;
        m_loss = loss;
        m_uniform = CreateObject<UniformRandomVariable>();
        m_uniform->SetStream(2002);
        m_dev = dev;
        m_limit = limit;
        std::ifstream in(file);
        NS_ABORT_MSG_UNLESS(in.good(), "Cannot open trace " << file);
        for (int64_t t; in >> t;)
        {
            m_ms.push_back(t);
        }
        NS_ABORT_MSG_IF(m_ms.empty() || m_ms.back() <= 0, "Unusable trace " << file);
        const int64_t period = m_ms.back();
        offsetMs %= period;
        m_next = std::lower_bound(m_ms.begin(), m_ms.end(), offsetMs) - m_ms.begin();
        m_baseMs = -offsetMs;
        ScheduleNext();
    }

    /** @return opportunities since the previous call */
    uint64_t TakeOpportunities()
    {
        const uint64_t c = m_opportunities;
        m_opportunities = 0;
        return c;
    }

  private:
    bool DoCorrupt(Ptr<Packet> p) override
    {
        if (m_reinject)
        {
            return false;
        }
        if (m_loss > 0 && m_uniform->GetValue() < m_loss)
        {
            return true;
        }
        if (m_delays.empty())
        {
            Enqueue(p->Copy());
        }
        else
        {
            const int64_t slot = Simulator::Now().GetMilliSeconds() / 10;
            // ns-3 TCP has no reordering tolerance (RACK), so a falling delay
            // must not overtake packets already on the way.
            m_lastArrival = std::max(m_lastArrival,
                                     Simulator::Now() +
                                         MilliSeconds(m_delays[slot % m_delays.size()]));
            Simulator::Schedule(m_lastArrival - Simulator::Now(),
                                &MahimahiGate::Enqueue,
                                this,
                                p->Copy());
        }
        return true;
    }

    void DoReset() override
    {
    }

    void Enqueue(Ptr<Packet> p)
    {
        if (m_bytes + p->GetSize() <= m_limit)
        {
            m_buffer.push_back(p);
            m_bytes += p->GetSize();
        }
    }

    void ScheduleNext()
    {
        if (m_next == m_ms.size())
        {
            m_baseMs += m_ms.back();
            m_next = 0;
        }
        // All the opportunities of one millisecond are served by one event.
        const int64_t ms = m_ms[m_next];
        m_batch = 0;
        while (m_next < m_ms.size() && m_ms[m_next] == ms)
        {
            m_next++;
            m_batch++;
        }
        const Time at = MilliSeconds(m_baseMs + ms);
        Simulator::ScheduleWithContext(m_dev->GetNode()->GetId(),
                                       at - Simulator::Now(),
                                       &MahimahiGate::Opportunity,
                                       this);
    }

    void Opportunity()
    {
        m_opportunities += m_batch;
        uint64_t left = 1500 * m_batch;
        while (left > 0 && !m_buffer.empty())
        {
            Ptr<Packet> p = m_buffer.front();
            // The 2-byte PPP header is not part of the 1500 bytes.
            const uint32_t need = p->GetSize() - PPP_HEADER - m_sentOfHead;
            const uint32_t now = std::min<uint64_t>(left, need);
            left -= now;
            m_sentOfHead += now;
            if (now == need)
            {
                m_buffer.pop_front();
                m_bytes -= p->GetSize();
                m_sentOfHead = 0;
                m_reinject = true;
                m_dev->Receive(p);
                m_reinject = false;
            }
        }
        ScheduleNext();
    }

    static constexpr uint32_t PPP_HEADER = 2; ///< Point-to-point header [bytes]
    Ptr<PointToPointNetDevice> m_dev;   ///< Gated device
    std::vector<int64_t> m_ms;          ///< Opportunities [ms]
    std::vector<int64_t> m_delays;      ///< One-way delay per 10 ms [ms]
    uint32_t m_batch{0};                ///< Opportunities of the next event
    Time m_lastArrival;                 ///< Latest arrival already scheduled
    double m_loss{0.0};                 ///< Random loss probability
    Ptr<UniformRandomVariable> m_uniform; ///< Draws the random loss
    std::deque<Ptr<Packet>> m_buffer;   ///< Waiting packets
    uint64_t m_bytes{0};                ///< Waiting bytes
    uint64_t m_limit{0};                ///< Buffer limit
    uint32_t m_sentOfHead{0};           ///< Bytes of the head packet already sent
    size_t m_next{0};                   ///< Next entry of the trace
    int64_t m_baseMs{0};                ///< Simulation time of trace time 0 [ms]
    uint64_t m_opportunities{0};        ///< Opportunities since the last query
    bool m_reinject{false};             ///< Delivering a packet
};

Ptr<PointToPointNetDevice> g_forwardDevice; ///< Router side of the bottleneck
Ptr<PointToPointNetDevice> g_reverseDevice; ///< Receiver side of the bottleneck
Ptr<RateErrorModel> g_dataLoss;             ///< Loss on data packets
Ptr<RateErrorModel> g_ackLoss;              ///< Loss on ACKs (outages only)
Ptr<HoldGate> g_dataGate;                   ///< Outage gate of the data direction
Ptr<HoldGate> g_ackGate;                    ///< Outage gate of the ACK direction
Ptr<MahimahiGate> g_mmDataGate;             ///< Trace gate of the data direction
Ptr<MahimahiGate> g_mmAckGate;              ///< Trace gate of the ACK direction
bool g_holdMode = false;                    ///< Buffer instead of drop during outages
double g_capacityMbps = 0.0;                ///< Current capacity
bool g_outage = false;                      ///< Current outage flag
Ptr<PointToPointChannel> g_bnChannel;       ///< Bottleneck channel
double g_accessDelayMs = 5.0;               ///< Access link one-way delay
double g_delayMs = 20.0;                    ///< Current bottleneck one-way delay
double g_delayTargetMs = 20.0;              ///< Bottleneck delay being slewed to
double g_delaySlewMs = 0.02;                ///< Largest delay decrease per millisecond
bool g_slewing = false;                     ///< A delay decrease is in progress

std::vector<Ptr<PacketSink>> g_sinks;           ///< One sink per flow
std::vector<uint64_t> g_lastRx;                 ///< Bytes at the last bin, per flow
std::vector<std::vector<double>> g_rttSamples;  ///< All RTT samples [ms], per flow
std::vector<std::vector<double>> g_srttSamples; ///< Smoothed RTT updates for diagnostics
std::ofstream g_rawRttLog; ///< Optional log of every valid ACK RTT sample
std::vector<double> g_binRttSum;                ///< RTT sum in the current bin, per flow
std::vector<uint32_t> g_binRttCount;            ///< RTT samples in the current bin
std::vector<double> g_binLastRttSum;            ///< Sum of RTT samples (last RTT) in the bin
std::vector<uint32_t> g_binLastRttCount;        ///< Number of RTT samples (last RTT) in the bin
std::vector<uint32_t> g_retx;                   ///< Retransmitted segments, per flow
uint32_t g_cwnd0 = 0;                           ///< Latest cwnd of flow 0
double g_binMs = 50.0;                          ///< Time-series bin
double g_warmup = 5.0;                          ///< Samples before this are not counted
double g_capacityIntegral = 0.0;                ///< Integral of capacity over the measured period [Mbit]
std::vector<uint64_t> g_rxAtWarmup;             ///< Bytes received at the end of warmup

std::ofstream g_series; ///< Time series output
std::ofstream g_events; ///< Event output
std::ofstream g_copaEst; ///< Estimates of the adaptive Copa delta (copa-auto, flow 0)

/**
 * Algorithm name without the Copa option suffixes "+nosw" (default mode only,
 * no switch to the competitive mode) and "+gcv" (velocity rule of the authors'
 * implementation genericCC), e.g. "copa-auto+nosw+gcv" -> "copa-auto".
 *
 * @param cc algorithm name
 * @return name without suffixes
 */
std::string
BaseCc(const std::string& cc)
{
    return cc.substr(0, cc.find('+'));
}

/**
 * Apply the Copa option suffixes of an algorithm name to the socket of a
 * sender node.
 *
 * @param nodeId sender node id
 * @param cc algorithm name with suffixes
 */
void
SetCopaOptions(uint32_t nodeId, std::string cc)
{
    const std::string path = "/NodeList/" + std::to_string(nodeId) +
                             "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpCopa/";
    if (cc.find("+nosw") != std::string::npos)
    {
        Config::Set(path + "ModeSwitching", BooleanValue(false));
    }
    if (cc.find("+gcv") != std::string::npos)
    {
        Config::Set(path + "GenericCcVelocity", BooleanValue(true));
    }
}

/**
 * Delta of a Copa variant name: "copa" is the default 0.5, "copa-d0.1" is 0.1.
 *
 * @param cc algorithm name
 * @return delta, or 0 if cc is not Copa
 */
double
CopaDelta(const std::string& name)
{
    const std::string cc = BaseCc(name);
    if (cc == "copa")
    {
        return 0.5;
    }
    if (cc == "copa-auto")
    {
        // Initial delta, until the online estimates are ready.
        return 0.1;
    }
    if (cc.rfind("copa-d", 0) == 0)
    {
        const double d = std::stod(cc.substr(6));
        NS_ABORT_MSG_UNLESS(d > 0 && d <= 1, "Copa delta must be in (0, 1]: " << cc);
        return d;
    }
    return 0;
}

/**
 * Set the delta of the Copa socket of a sender node, right after the
 * application has created it.
 *
 * @param nodeId sender node id
 * @param delta Copa delta (latency factor)
 */
void
SetCopaDelta(uint32_t nodeId, double delta)
{
    Config::Set("/NodeList/" + std::to_string(nodeId) +
                    "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpCopa/LatencyFactor",
                DoubleValue(delta));
}

/**
 * Record the online estimates of copa-auto.
 *
 * @param jitter jitter floor j [s]
 * @param ratio k = c / mu [s]
 * @param rho utilization estimate
 * @param baseRtt R0' [s]
 * @param dqTarget target queuing delay [s]
 * @param delta delta in use
 */
void
CopaEstimateTracer(double jitter, double ratio, double rho, double baseRtt, double dqTarget, double delta)
{
    g_copaEst << Simulator::Now().GetSeconds() << " " << jitter * 1e3 << " " << ratio * 1e3 << " "
              << rho << " " << baseRtt * 1e3 << " " << dqTarget * 1e3 << " " << delta << "\n";
}

/**
 * Turn on the adaptive delta of the Copa socket of a sender node; the
 * estimates of flow 0 are recorded.
 *
 * @param nodeId sender node id
 * @param flow flow index
 */
void
EnableAdaptiveCopa(uint32_t nodeId, uint32_t flow)
{
    const std::string path = "/NodeList/" + std::to_string(nodeId) +
                             "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpCopa/";
    Config::Set(path + "AdaptiveDelta", BooleanValue(true));
    if (flow == 0)
    {
        Config::ConnectWithoutContext(path + "AdaptiveEstimate",
                                      MakeCallback(&CopaEstimateTracer));
    }
}

/**
 * Map a short algorithm name to its ns-3 TypeId name.
 *
 * @param cc short name
 * @return TypeId name
 */
std::string
CcTypeName(const std::string& name)
{
    const std::string cc = BaseCc(name);
    if (cc == "cubic")
    {
        return "ns3::TcpCubic";
    }
    if (cc == "bbr")
    {
        return "ns3::TcpBbr";
    }
    if (cc == "bbrv2" || cc == "bbrv2l")
    {
        return "ns3::TcpBbrV2";
    }
    if (cc == "bbrv3" || cc == "bbrv3l")
    {
        return "ns3::TcpBbrV3";
    }
    if (cc == "copa" || cc == "copa-auto" || cc.rfind("copa-d", 0) == 0)
    {
        return "ns3::TcpCopa";
    }
    if (cc == "newreno")
    {
        return "ns3::TcpNewReno";
    }
    // Allow full TypeId names for algorithms added later.
    TypeId tid;
    NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(cc, &tid),
                        "Unknown congestion control '" << cc << "'");
    return cc;
}

/**
 * Nearest-rank percentile.
 *
 * @param v samples (sorted in place)
 * @param p percentile in [0, 100]
 * @return percentile, 0 when empty
 */
double
Percentile(std::vector<double>& v, double p)
{
    if (v.empty())
    {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    auto idx = static_cast<size_t>(std::ceil(p / 100.0 * v.size()));
    idx = std::min(std::max<size_t>(idx, 1), v.size()) - 1;
    return v[idx];
}

/**
 * Set the bottleneck one-way delay.
 *
 * @param ms delay [ms]
 */
void
SetBottleneckDelay(double ms)
{
    g_delayMs = ms;
    g_bnChannel->SetAttribute("Delay", TimeValue(MicroSeconds(std::llround(ms * 1000.0))));
}

/**
 * Move the bottleneck delay towards its target by at most --delaySlew per
 * millisecond. A sudden decrease would let later packets overtake earlier
 * ones and trigger spurious fast retransmits; as long as the step is smaller
 * than the packet spacing, the order is kept.
 */
void
SlewDelay()
{
    if (g_delayMs - g_delayTargetMs <= g_delaySlewMs)
    {
        SetBottleneckDelay(g_delayTargetMs);
        g_slewing = false;
        return;
    }
    SetBottleneckDelay(g_delayMs - g_delaySlewMs);
    Simulator::Schedule(MilliSeconds(1), &SlewDelay);
}

/**
 * Apply one capacity segment to the bottleneck.
 *
 * @param seg segment
 */
void
ApplySegment(Segment seg)
{
    const bool wasOutage = g_outage;
    if (seg.delayMs >= 0)
    {
        g_delayTargetMs = seg.delayMs;
        if (seg.delayMs >= g_delayMs)
        {
            SetBottleneckDelay(seg.delayMs);
        }
        else if (!g_slewing)
        {
            g_slewing = true;
            SlewDelay();
        }
    }
    g_outage = seg.loss >= 1.0;
    g_capacityMbps = g_outage ? 0.0 : seg.rateMbps;
    if (!g_outage)
    {
        DataRate rate(static_cast<uint64_t>(seg.rateMbps * 1e6));
        g_forwardDevice->SetAttribute("DataRate", DataRateValue(rate));
        g_reverseDevice->SetAttribute("DataRate", DataRateValue(rate));
        g_dataLoss->SetRate(std::min(seg.loss, 1.0));
        if (wasOutage)
        {
            g_dataGate->EndOutage(rate);
            g_ackGate->EndOutage(rate);
        }
    }
    else if (!wasOutage)
    {
        g_dataGate->StartOutage(g_holdMode);
        g_ackGate->StartOutage(g_holdMode);
    }
    if (!seg.event.empty())
    {
        g_events << std::fixed << std::setprecision(4) << Simulator::Now().GetSeconds() << ","
                 << seg.event << "," << seg.rateMbps << std::endl;
    }
}

/** Write one time-series row and reschedule. */
void
SampleBin()
{
    const double now = Simulator::Now().GetSeconds();
    const double dt = g_binMs / 1000.0;
    if (g_mmDataGate)
    {
        g_capacityMbps = g_mmDataGate->TakeOpportunities() * 12000.0 / dt / 1e6;
    }
    if (now > g_warmup + 1e-9)
    {
        g_capacityIntegral += g_capacityMbps * dt;
    }
    g_series << std::fixed << std::setprecision(3) << now << "," << g_capacityMbps << ","
             << (g_outage ? 1 : 0);
    for (size_t i = 0; i < g_sinks.size(); i++)
    {
        uint64_t rx = g_sinks[i]->GetTotalRx();
        g_series << "," << (rx - g_lastRx[i]) * 8.0 / dt / 1e6;
        g_lastRx[i] = rx;
    }
    for (size_t i = 0; i < g_sinks.size(); i++)
    {
        double rtt = g_binRttCount[i] ? g_binRttSum[i] / g_binRttCount[i] : 0.0;
        g_series << "," << rtt;
        g_binRttSum[i] = 0.0;
        g_binRttCount[i] = 0;
    }
    g_series << "," << g_cwnd0;
    for (size_t i = 0; i < g_sinks.size(); i++)
    {
        g_series << ","
                 << (g_binLastRttCount[i] ? g_binLastRttSum[i] / g_binLastRttCount[i] : 0.0);
        g_binLastRttSum[i] = 0.0;
        g_binLastRttCount[i] = 0;
    }
    g_series << "," << 2 * (g_delayMs + g_accessDelayMs) << "\n";
    Simulator::Schedule(MilliSeconds(g_binMs), &SampleBin);
}

/** Snapshot the received bytes at the end of the warmup period. */
void
EndWarmup()
{
    for (size_t i = 0; i < g_sinks.size(); i++)
    {
        g_rxAtWarmup[i] = g_sinks[i]->GetTotalRx();
    }
}

/**
 * Accumulate an RTT sample (RTT of the last acknowledged segment) of one flow.
 *
 * @param flow flow index
 * @param oldVal previous value
 * @param newVal new RTT sample
 */
void
LastRttTrace(uint32_t flow, Time newVal)
{
    if (newVal.IsStrictlyPositive())
    {
        g_binLastRttSum[flow] += newVal.GetSeconds() * 1000.0;
        g_binLastRttCount[flow]++;
        if (Simulator::Now().GetSeconds() > g_warmup)
        {
            g_rttSamples[flow].push_back(newVal.GetSeconds() * 1000.0);
            if (g_rawRttLog.is_open())
            {
                g_rawRttLog << std::setprecision(12) << Simulator::Now().GetSeconds() << ","
                            << flow << "," << newVal.GetSeconds() * 1000.0 << "\n";
            }
        }
    }
}

/**
 * Record an RTT sample of one flow.
 *
 * @param flow flow index
 * @param oldVal previous smoothed RTT
 * @param newVal new smoothed RTT
 */
void
RttTrace(uint32_t flow, Time oldVal [[maybe_unused]], Time newVal)
{
    double ms = newVal.GetSeconds() * 1000.0;
    if (ms <= 0)
    {
        return;
    }
    g_binRttSum[flow] += ms;
    g_binRttCount[flow]++;
    if (Simulator::Now().GetSeconds() > g_warmup)
    {
        g_srttSamples[flow].push_back(ms);
    }
}

/**
 * Record the cwnd of flow 0.
 *
 * @param oldVal previous cwnd
 * @param newVal new cwnd
 */
void
CwndTrace(uint32_t oldVal [[maybe_unused]], uint32_t newVal)
{
    g_cwnd0 = newVal;
}

/**
 * Count a retransmitted segment.
 *
 * @param flow flow index
 * @param packet packet
 * @param header TCP header
 * @param local local address
 * @param peer peer address
 * @param socket socket
 */
void
RetxTrace(uint32_t flow,
          Ptr<const Packet> packet [[maybe_unused]],
          const TcpHeader& header [[maybe_unused]],
          const Address& local [[maybe_unused]],
          const Address& peer [[maybe_unused]],
          Ptr<const TcpSocketBase> socket [[maybe_unused]])
{
    if (Simulator::Now().GetSeconds() > g_warmup)
    {
        g_retx[flow]++;
    }
}

/**
 * Connect the traces of one sender once its socket exists.
 *
 * @param nodeId sender node id
 * @param flow flow index
 */
void
ConnectTraces(uint32_t nodeId, uint32_t flow)
{
    std::string path =
        "/NodeList/" + std::to_string(nodeId) + "/$ns3::TcpL4Protocol/SocketList/0/";
    Config::ConnectWithoutContext(path + "RTT", MakeBoundCallback(&RttTrace, flow));
    Config::ConnectWithoutContext(path + "RttSample", MakeBoundCallback(&LastRttTrace, flow));
    Config::ConnectWithoutContext(path + "Retransmission", MakeBoundCallback(&RetxTrace, flow));
    if (flow == 0)
    {
        Config::ConnectWithoutContext(path + "CongestionWindow", MakeCallback(&CwndTrace));
    }
}

/**
 * Split a comma separated list.
 *
 * @param s input
 * @return items
 */
std::vector<std::string>
Split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        if (!item.empty())
        {
            out.push_back(item);
        }
    }
    return out;
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string ccs = "bbrv2";
    std::string profile = "outage";
    std::string traceFile;
    std::string downTrace;
    std::string upTrace;
    std::string delayTrace;
    double traceOffset = 0.0;
    std::string prefix = "tcp-capacity";
    bool rttSampleLog = false;
    double stopTime = 60.0;
    double capacity = 20.0; // Mbit/s
    double capMin = 5.0;
    double capMax = 40.0;
    double outageAt = 20.0;
    double outageDuration = 3.0;
    double stepFactor = 0.25;
    double stepAt = 20.0;
    double stepDuration = 15.0;
    double hoInterval = 5.0;
    double hoGapMs = 80.0;
    double hoLoss = 0.0;
    double fluctPeriod = 0.5;
    double fluctSigma = 0.3;
    double randomLoss = 0.0;
    std::string disruption = "drop";
    uint64_t holdBytes = 0;
    double eventJitter = 2.0;
    double bottleneckDelayMs = 20.0;
    double accessDelayMs = 5.0;
    uint32_t queueBytes = 500000;
    std::string aqm = "none";
    double minRtoMs = 200.0;
    double flowStagger = 0.0;
    uint32_t segmentSize = 1448;
    uint32_t sndBufBytes = 1U << 26;
    uint32_t seed = 1;
    uint32_t run = 1;
    bool freshLoss = true;

    CommandLine cmd(__FILE__);
    cmd.AddValue("ccs",
                 "Comma separated algorithm of every flow, e.g. bbrv2l,cubic; copa is Copa "
                 "with delta 0.5, copa-d<delta> sets another delta",
                 ccs);
    cmd.AddValue("profile", "Capacity profile: outage|step|handover|fluctuate|file", profile);
    cmd.AddValue("traceFile",
                 "Schedule for --profile=file: 'time_s rate_mbps [loss [delay_ms]]'",
                 traceFile);
    cmd.AddValue("downTrace", "Mahimahi trace of the data direction (mahimahi)", downTrace);
    cmd.AddValue("upTrace", "Mahimahi trace of the ACK direction, empty = unlimited", upTrace);
    cmd.AddValue("delayTrace", "One-way delay per 10 ms [ms], both directions (mahimahi)", delayTrace);
    cmd.AddValue("traceOffset", "Trace position at simulation time 0 [s] (mahimahi)", traceOffset);
    cmd.AddValue("prefix", "Prefix of the output files", prefix);
    cmd.AddValue("stopTime", "Simulation duration [s]", stopTime);
    cmd.AddValue("warmup", "Initial period excluded from the metrics [s]", g_warmup);
    cmd.AddValue("binMs", "Time-series bin [ms]", g_binMs);
    cmd.AddValue("capacity", "Nominal capacity [Mbit/s] (outage, step)", capacity);
    cmd.AddValue("capMin", "Minimum capacity [Mbit/s] (handover, fluctuate)", capMin);
    cmd.AddValue("capMax", "Maximum capacity [Mbit/s] (handover, fluctuate)", capMax);
    cmd.AddValue("outageAt", "Outage start [s]", outageAt);
    cmd.AddValue("outageDuration", "Outage duration [s]", outageDuration);
    cmd.AddValue("stepFactor", "Capacity multiplier during the step", stepFactor);
    cmd.AddValue("stepAt", "Step start [s]", stepAt);
    cmd.AddValue("stepDuration", "Step duration [s]", stepDuration);
    cmd.AddValue("hoInterval", "Mean time between handovers [s]", hoInterval);
    cmd.AddValue("hoGap", "Interruption of each handover [ms]", hoGapMs);
    cmd.AddValue("hoLoss", "Extra data loss right after a handover (1 s)", hoLoss);
    cmd.AddValue("fluctPeriod", "Capacity update period of the random walk [s]", fluctPeriod);
    cmd.AddValue("fluctSigma", "Log-normal step of the random walk", fluctSigma);
    cmd.AddValue("randomLoss", "Random data loss probability outside outages", randomLoss);
    cmd.AddValue("disruption",
                 "What an outage does to packets: drop (coverage hole) | hold (buffered "
                 "handover / RLC AM)",
                 disruption);
    cmd.AddValue("holdBytes", "Buffer of the hold mode [bytes], 0 = --queueBytes", holdBytes);
    cmd.AddValue("eventJitter", "Uniform +/- jitter of event times [s]", eventJitter);
    cmd.AddValue("bottleneckDelay", "Bottleneck one-way delay [ms]", bottleneckDelayMs);
    cmd.AddValue("accessDelay", "Access link one-way delay [ms]", accessDelayMs);
    cmd.AddValue("delaySlew",
                 "Largest decrease of the bottleneck delay per millisecond [ms] (file profile)",
                 g_delaySlewMs);
    cmd.AddValue("queueBytes", "Bottleneck buffer [bytes]", queueBytes);
    cmd.AddValue("aqm", "Bottleneck queue discipline: none (FIFO) | fqcodel", aqm);
    cmd.AddValue("minRto", "TCP minimum RTO [ms]", minRtoMs);
    cmd.AddValue("flowStagger", "Start offset between consecutive flows [s]", flowStagger);
    cmd.AddValue("segmentSize", "TCP segment size [bytes]", segmentSize);
    cmd.AddValue("sndBuf", "TCP send buffer [bytes]", sndBufBytes);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("run", "RNG run number", run);
    cmd.AddValue("rttSampleLog", "Write every valid ACK RTT sample to CSV", rttSampleLog);
    cmd.AddValue("freshLoss",
                 "Rate samples report only freshly lost data (TcpSocketBase::FreshLossAccounting)",
                 freshLoss);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_UNLESS(disruption == "drop" || disruption == "hold",
                        "--disruption must be drop or hold");
    g_holdMode = disruption == "hold";
    if (holdBytes == 0)
    {
        holdBytes = queueBytes;
    }
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    const std::vector<std::string> flows = Split(ccs);
    NS_ABORT_MSG_IF(flows.empty(), "--ccs is empty");
    // "bbrv2l"/"bbrv3l" = BBRv2/v3 with the Linux loss response. It is a
    // TypeId default, so it cannot be mixed with the original model.
    bool linux = false;
    bool original = false;
    for (const auto& f : flows)
    {
        linux |= (f == "bbrv2l" || f == "bbrv3l");
        original |= (f == "bbrv2" || f == "bbrv3");
    }
    NS_ABORT_MSG_IF(linux && original, "bbrv2l/bbrv3l cannot be mixed with bbrv2/bbrv3");
    Config::SetDefault("ns3::TcpBbrV2::LinuxLossResponse", BooleanValue(linux));
    const uint32_t n = flows.size();

    // ---- Capacity schedule -------------------------------------------------
    Ptr<UniformRandomVariable> uni = CreateObject<UniformRandomVariable>();
    uni->SetStream(1000);
    Ptr<NormalRandomVariable> gauss = CreateObject<NormalRandomVariable>();
    gauss->SetStream(1001);
    auto jit = [&](double t) {
        return t + (eventJitter > 0 ? uni->GetValue(-eventJitter, eventJitter) : 0.0);
    };

    std::vector<Segment> sched;
    if (profile == "outage")
    {
        double t = jit(outageAt);
        sched.push_back({0.0, capacity, randomLoss, "start"});
        if (outageDuration > 0)
        {
            sched.push_back({t, capacity, 1.0, "outage-begin"});
            sched.push_back({t + outageDuration, capacity, randomLoss, "outage-end"});
        }
    }
    else if (profile == "step")
    {
        double t = jit(stepAt);
        sched.push_back({0.0, capacity, randomLoss, "start"});
        sched.push_back({t, capacity * stepFactor, randomLoss, "step-down"});
        sched.push_back({t + stepDuration, capacity, randomLoss, "step-up"});
    }
    else if (profile == "handover")
    {
        double cap = uni->GetValue(capMin, capMax);
        sched.push_back({0.0, cap, randomLoss, "start"});
        double t = g_warmup + uni->GetValue(0.5, 1.5) * hoInterval;
        while (t < stopTime - 1.0)
        {
            sched.push_back({t, cap, 1.0, "outage-begin"});
            cap = uni->GetValue(capMin, capMax);
            double end = t + hoGapMs / 1000.0;
            if (hoLoss > 0)
            {
                sched.push_back({end, cap, hoLoss, "cell-change"});
                sched.push_back({end + 1.0, cap, randomLoss, ""});
            }
            else
            {
                sched.push_back({end, cap, randomLoss, "cell-change"});
            }
            t += uni->GetValue(0.5, 1.5) * hoInterval;
        }
    }
    else if (profile == "fluctuate")
    {
        double cap = std::sqrt(capMin * capMax);
        sched.push_back({0.0, cap, randomLoss, "start"});
        for (double t = fluctPeriod; t < stopTime; t += fluctPeriod)
        {
            cap = std::min(capMax, std::max(capMin, cap * std::exp(fluctSigma * gauss->GetValue())));
            sched.push_back({t, cap, randomLoss, "rate-change"});
        }
    }
    else if (profile == "mahimahi")
    {
        // The devices only have to outrun the trace.
        sched.push_back({0.0, 10000.0, 0.0, "start"});
    }
    else if (profile == "file")
    {
        std::ifstream in(traceFile);
        NS_ABORT_MSG_UNLESS(in.good(), "Cannot open --traceFile " << traceFile);
        std::string line;
        bool prevOutage = false;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
            {
                continue;
            }
            std::istringstream ls(line);
            double t;
            double r;
            double l = randomLoss;
            double d = -1.0;
            if (!(ls >> t >> r))
            {
                continue;
            }
            if (ls >> l)
            {
                ls >> d;
            }
            else
            {
                l = randomLoss;
            }
            bool out = l >= 1.0 || r <= 0.0;
            std::string ev = out ? (prevOutage ? "" : "outage-begin")
                                 : (prevOutage ? "outage-end" : "rate-change");
            Segment seg{t, std::max(r, 0.001), out ? 1.0 : l, ev};
            seg.delayMs = d;
            sched.push_back(seg);
            prevOutage = out;
        }
        NS_ABORT_MSG_IF(sched.empty(), "Empty --traceFile");
        if (sched[0].delayMs >= 0)
        {
            bottleneckDelayMs = sched[0].delayMs;
        }
    }
    else
    {
        NS_FATAL_ERROR("Unknown profile '" << profile << "'");
    }

    // ---- TCP defaults ------------------------------------------------------
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(segmentSize));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(sndBufBytes));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(1U << 26));
    Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(true));
    Config::SetDefault("ns3::TcpSocketBase::MinRto", TimeValue(MilliSeconds(minRtoMs)));
    Config::SetDefault("ns3::TcpSocketBase::FreshLossAccounting", BooleanValue(freshLoss));
    // Linux gives up after ~15 min of retries; ns-3's 6 retries would drop a
    // sender that probes a broken path at a fixed interval.
    Config::SetDefault("ns3::TcpSocket::DataRetries", UintegerValue(100));
    Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(true));
    Config::SetDefault("ns3::TcpSocketState::MaxPacingRate", DataRateValue(DataRate("10Gbps")));
    Config::SetDefault("ns3::TcpL4Protocol::RecoveryType",
                       TypeIdValue(TcpPrrRecovery::GetTypeId()));

    // ---- Topology ----------------------------------------------------------
    NodeContainer senders;
    senders.Create(n);
    Ptr<Node> router = CreateObject<Node>();
    Ptr<Node> receiver = CreateObject<Node>();

    InternetStackHelper internet;
    internet.Install(senders);
    internet.Install(router);
    internet.Install(receiver);
    for (uint32_t i = 0; i < n; i++)
    {
        senders.Get(i)->GetObject<TcpL4Protocol>()->SetAttribute(
            "SocketType",
            TypeIdValue(TypeId::LookupByName(CcTypeName(flows[i]))));
    }

    PointToPointHelper access;
    access.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    access.SetChannelAttribute("Delay", TimeValue(MilliSeconds(accessDelayMs)));

    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate",
                                  DataRateValue(DataRate(static_cast<uint64_t>(
                                      std::max(sched[0].rateMbps, 0.001) * 1e6))));
    bottleneck.SetChannelAttribute("Delay", TimeValue(MilliSeconds(bottleneckDelayMs)));
    // Small device queue: the buffer lives in the queue disc below.
    bottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("5p"));
    NetDeviceContainer bnDevs = bottleneck.Install(router, receiver);

    TrafficControlHelper tch;
    if (aqm == "none")
    {
        tch.SetRootQueueDisc("ns3::FifoQueueDisc",
                             "MaxSize",
                             QueueSizeValue(QueueSize(QueueSizeUnit::BYTES, queueBytes)));
    }
    else if (aqm == "fqcodel")
    {
        // FQ-CoDel only supports a limit in packets.
        tch.SetRootQueueDisc(
            "ns3::FqCoDelQueueDisc",
            "MaxSize",
            QueueSizeValue(QueueSize(QueueSizeUnit::PACKETS,
                                     std::max<uint32_t>(queueBytes / (segmentSize + 52), 10))));
    }
    else
    {
        NS_FATAL_ERROR("Unknown --aqm '" << aqm << "'");
    }
    tch.Install(bnDevs.Get(0));

    Ipv4AddressHelper addr;
    for (uint32_t i = 0; i < n; i++)
    {
        NetDeviceContainer d = access.Install(senders.Get(i), router);
        std::ostringstream net;
        net << "10.1." << i << ".0";
        addr.SetBase(net.str().c_str(), "255.255.255.0");
        addr.Assign(d);
    }
    addr.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer bnIfaces = addr.Assign(bnDevs);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    g_forwardDevice = DynamicCast<PointToPointNetDevice>(bnDevs.Get(0));
    g_bnChannel = DynamicCast<PointToPointChannel>(g_forwardDevice->GetChannel());
    g_accessDelayMs = accessDelayMs;
    g_delayMs = bottleneckDelayMs;
    g_delayTargetMs = bottleneckDelayMs;
    g_reverseDevice = DynamicCast<PointToPointNetDevice>(bnDevs.Get(1));
    g_dataLoss = CreateObject<RateErrorModel>();
    g_dataLoss->SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
    g_dataLoss->SetRate(0.0);
    g_dataLoss->AssignStreams(2000);
    g_dataGate = CreateObject<HoldGate>();
    g_dataGate->Setup(g_reverseDevice, g_dataLoss, holdBytes);
    g_reverseDevice->SetReceiveErrorModel(g_dataGate);
    g_ackLoss = CreateObject<RateErrorModel>();
    g_ackLoss->SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
    g_ackLoss->SetRate(0.0);
    g_ackLoss->AssignStreams(2001);
    g_ackGate = CreateObject<HoldGate>();
    g_ackGate->Setup(g_forwardDevice, g_ackLoss, holdBytes);
    g_forwardDevice->SetReceiveErrorModel(g_ackGate);
    if (profile == "mahimahi")
    {
        const int64_t offsetMs = static_cast<int64_t>(traceOffset * 1000);
        std::vector<int64_t> delays;
        if (!delayTrace.empty())
        {
            std::ifstream in(delayTrace);
            NS_ABORT_MSG_UNLESS(in.good(), "Cannot open trace " << delayTrace);
            for (int64_t d; in >> d;)
            {
                delays.push_back(d);
            }
            NS_ABORT_MSG_IF(delays.empty(), "Unusable trace " << delayTrace);
        }
        g_mmDataGate = CreateObject<MahimahiGate>();
        g_mmDataGate->Setup(g_reverseDevice, downTrace, queueBytes, offsetMs, delays, randomLoss);
        g_reverseDevice->SetReceiveErrorModel(g_mmDataGate);
        if (!upTrace.empty())
        {
            g_mmAckGate = CreateObject<MahimahiGate>();
            g_mmAckGate->Setup(g_forwardDevice, upTrace, queueBytes, offsetMs, delays, 0.0);
            g_forwardDevice->SetReceiveErrorModel(g_mmAckGate);
        }
    }

    // ---- Applications ------------------------------------------------------
    const uint16_t basePort = 5000;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint16_t port = basePort + i;
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sinkApp = sinkHelper.Install(receiver);
        sinkApp.Start(Seconds(0));
        g_sinks.push_back(DynamicCast<PacketSink>(sinkApp.Get(0)));

        BulkSendHelper bulk("ns3::TcpSocketFactory",
                            InetSocketAddress(bnIfaces.GetAddress(1), port));
        bulk.SetAttribute("MaxBytes", UintegerValue(0));
        ApplicationContainer src = bulk.Install(senders.Get(i));
        const double start = 0.1 + i * flowStagger;
        src.Start(Seconds(start));
        Simulator::Schedule(Seconds(start + 0.001),
                            &ConnectTraces,
                            senders.Get(i)->GetId(),
                            i);
        if (CopaDelta(flows[i]) > 0)
        {
            Simulator::Schedule(Seconds(start) + MicroSeconds(1),
                                &SetCopaDelta,
                                senders.Get(i)->GetId(),
                                CopaDelta(flows[i]));
        }
        if (CopaDelta(flows[i]) > 0 && flows[i].find('+') != std::string::npos)
        {
            Simulator::Schedule(Seconds(start) + MicroSeconds(1),
                                &SetCopaOptions,
                                senders.Get(i)->GetId(),
                                flows[i]);
        }
        if (BaseCc(flows[i]) == "copa-auto")
        {
            if (!g_copaEst.is_open())
            {
                g_copaEst.open(prefix + "-copaest.data");
                g_copaEst << "# t j_ms k_ms rho r0_ms dq_target_ms delta\n";
            }
            Simulator::Schedule(Seconds(start) + MicroSeconds(2),
                                &EnableAdaptiveCopa,
                                senders.Get(i)->GetId(),
                                i);
        }
    }
    g_lastRx.assign(n, 0);
    g_rxAtWarmup.assign(n, 0);
    g_rttSamples.assign(n, {});
    g_srttSamples.assign(n, {});
    g_binRttSum.assign(n, 0.0);
    g_binRttCount.assign(n, 0);
    g_binLastRttSum.assign(n, 0.0);
    g_binLastRttCount.assign(n, 0);
    g_retx.assign(n, 0);

    // ---- Outputs and schedule ---------------------------------------------
    g_series.open(prefix + "-timeseries.csv");
    if (rttSampleLog)
    {
        g_rawRttLog.open(prefix + "-rtt-samples.csv");
        g_rawRttLog << "time_s,flow,rtt_ms\n";
    }
    g_series << "time_s,capacity_mbps,outage";
    for (uint32_t i = 0; i < n; i++)
    {
        g_series << ",tput" << i << "_mbps";
    }
    for (uint32_t i = 0; i < n; i++)
    {
        g_series << ",rtt" << i << "_ms";
    }
    g_series << ",cwnd0_bytes";
    // rtt<i>_ms is the smoothed RTT, lrtt<i>_ms the mean RTT sample of the bin.
    for (uint32_t i = 0; i < n; i++)
    {
        g_series << ",lrtt" << i << "_ms";
    }
    g_series << ",base_rtt_ms\n";
    g_events.open(prefix + "-events.csv");
    g_events << "time_s,event,capacity_mbps\n";

    for (const auto& seg : sched)
    {
        if (seg.start < stopTime)
        {
            Simulator::Schedule(Seconds(seg.start), &ApplySegment, seg);
        }
    }
    Simulator::Schedule(MilliSeconds(g_binMs), &SampleBin);
    Simulator::Schedule(Seconds(g_warmup), &EndWarmup);

    std::cout << "capacity-trace: ccs=" << ccs << " profile=" << profile << " run=" << run
              << " queue=" << queueBytes << "B aqm=" << aqm << " baseRTT="
              << 2 * (bottleneckDelayMs + accessDelayMs) << "ms minRto=" << minRtoMs << "ms"
              << std::endl;

    Simulator::Stop(Seconds(stopTime));
    Simulator::Run();

    // ---- Summary -----------------------------------------------------------
    const double measured = stopTime - g_warmup;
    double sum = 0.0;
    double sumSq = 0.0;
    std::vector<double> tput(n);
    for (uint32_t i = 0; i < n; i++)
    {
        tput[i] = (g_sinks[i]->GetTotalRx() - g_rxAtWarmup[i]) * 8.0 / measured / 1e6;
        sum += tput[i];
        sumSq += tput[i] * tput[i];
    }
    const double avgCap = g_capacityIntegral / measured;
    const double jain = sumSq > 0 ? sum * sum / (n * sumSq) : 0.0;

    std::ofstream json(prefix + "-metrics.json");
    json << std::fixed << std::setprecision(4);
    json << "{\n  \"rtt_stats_version\": 2, \"rtt_source\": \"ack_rtt_samples\",\n"
         << "  \"ccs\": \"" << ccs << "\", \"profile\": \"" << profile
         << "\", \"run\": " << run << ", \"queue_bytes\": " << queueBytes << ", \"aqm\": \""
         << aqm << "\", \"disruption\": \"" << disruption
         << "\", \"hold_overflow_bytes\": " << g_dataGate->GetOverflowBytes()
         << ", \"base_rtt_ms\": " << 2 * (bottleneckDelayMs + accessDelayMs)
         << ", \"min_rto_ms\": " << minRtoMs << ",\n  \"avg_capacity_mbps\": " << avgCap
         << ", \"total_throughput_mbps\": " << sum
         << ", \"utilization\": " << (avgCap > 0 ? sum / avgCap : 0.0) << ", \"jain\": " << jain
         << ",\n  \"flows\": [\n";
    for (uint32_t i = 0; i < n; i++)
    {
        std::vector<double> r = g_rttSamples[i];
        double mean = 0.0;
        for (double x : r)
        {
            mean += x;
        }
        mean = r.empty() ? 0.0 : mean / r.size();
        double srttMean = 0.0;
        for (double x : g_srttSamples[i])
        {
            srttMean += x;
        }
        srttMean = g_srttSamples[i].empty() ? 0.0 : srttMean / g_srttSamples[i].size();
        double p50 = Percentile(r, 50);
        double p95 = Percentile(r, 95);
        double p99 = Percentile(r, 99);
        json << "    {\"flow\": " << i << ", \"cc\": \"" << flows[i]
             << "\", \"throughput_mbps\": " << tput[i] << ", \"retx\": " << g_retx[i]
             << ", \"srtt_mean_ms\": " << srttMean << ", \"rtt_samples\": " << r.size()
             << ", \"rtt_mean_ms\": " << mean << ", \"rtt_p50_ms\": " << p50
             << ", \"rtt_p95_ms\": " << p95 << ", \"rtt_p99_ms\": " << p99 << "}"
             << (i + 1 < n ? "," : "") << "\n";
        std::cout << "flow " << i << " " << flows[i] << ": " << std::setprecision(3) << tput[i]
                  << " Mbps, rtt p95 " << p95 << " ms, retx " << g_retx[i] << std::endl;
    }
    std::vector<double> pooled;
    for (const auto& samples : g_rttSamples)
    {
        pooled.insert(pooled.end(), samples.begin(), samples.end());
    }
    json << "  ],\n  \"rtt_p95_ms\": " << Percentile(pooled, 95)
         << ", \"rtt_p99_ms\": " << Percentile(pooled, 99)
         << ", \"rtt_samples\": " << pooled.size() << "\n}\n";
    std::cout << "capacity " << avgCap << " Mbps, utilisation " << (avgCap > 0 ? sum / avgCap : 0)
              << ", Jain " << jain << std::endl;

    Simulator::Destroy();
    return 0;
}
