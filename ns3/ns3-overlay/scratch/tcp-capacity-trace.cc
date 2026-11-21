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

    void SetStatsWarmupS(double s)
    {
        m_statsWarmupS = s;
    }

    const std::vector<double>& GetQDelaysMs() const
    {
        return m_qdelaysMs;
    }

    uint64_t GetDeliveredPkts() const
    {
        return m_deliveredPkts;
    }

    uint64_t GetDroppedPkts() const
    {
        return m_droppedPkts;
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
            m_enqTimes.push_back(Simulator::Now());
            m_bytes += p->GetSize();
        }
        else if (Simulator::Now().GetSeconds() >= m_statsWarmupS)
        {
            m_droppedPkts++;
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
                Time enq = m_enqTimes.front();
                m_enqTimes.pop_front();
                m_bytes -= p->GetSize();
                m_sentOfHead = 0;
                if (Simulator::Now().GetSeconds() >= m_statsWarmupS)
                {
                    m_qdelaysMs.push_back((Simulator::Now() - enq).GetSeconds() * 1000.0);
                    m_deliveredPkts++;
                }
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
    std::deque<Time> m_enqTimes;        ///< Enqueue timestamps matching m_buffer
    std::vector<double> m_qdelaysMs;    ///< Per-packet queuing delays after warmup [ms]
    double m_statsWarmupS{0.0};         ///< Warmup cutoff for qdelay/drop stats [s]
    uint64_t m_deliveredPkts{0};        ///< Packets delivered after warmup
    uint64_t m_droppedPkts{0};          ///< Packets dropped after warmup
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
std::vector<std::vector<double>> g_baseRttSamples; ///< Path base RTT at each RTT sample
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

bool
IsCopaKernelName(const std::string& name)
{
    const std::string cc = BaseCc(name);
    return cc == "copa" || cc == "copa-auto" || cc == "copa_auto" || cc == "copa_auto_nc" ||
           cc == "copa_auto_nc_j0" || cc == "copa_auto_nc_j3" || cc == "copa_orig" ||
           cc.rfind("copa-d", 0) == 0 || cc.rfind("copa_auto_theta", 0) == 0 ||
           cc.rfind("copa_auto_nc_theta", 0) == 0;
}

bool
IsAdaptiveCopaName(const std::string& name)
{
    const std::string cc = BaseCc(name);
    return cc == "copa-auto-v2" || cc == "copa_auto_v2" || cc == "copa-auto" || cc == "copa_auto" || cc == "copa_auto_nc" ||
           cc == "copa_auto_nc_j0" || cc == "copa_auto_nc_j3" ||
           cc.rfind("copa_auto_theta", 0) == 0 || cc.rfind("copa_auto_nc_theta", 0) == 0;
}

/**
 * Record the online estimates of copa-auto.
 */
void
CopaEstimateTracer(double jitter, double ratio, double rho, double baseRtt, double dqTarget, double delta)
{
    if (g_copaEst.is_open())
    {
        g_copaEst << Simulator::Now().GetSeconds() << " " << jitter * 1e3 << " " << ratio * 1e3
                  << " " << rho << " " << baseRtt * 1e3 << " " << dqTarget * 1e3 << " " << delta
                  << "\n";
    }
}

void
ConfigureCcFlow(uint32_t nodeId,
                uint32_t flow,
                std::string name,
                int32_t jitterUs,
                uint32_t copaClockUs,
                uint32_t debugMs)
{
    const std::string cc = BaseCc(name);
    if (cc == "mortise_copa")
    {
        const std::string path = "/NodeList/" + std::to_string(nodeId) +
                                 "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpMortiseCopa/";
        Config::Set(path + "CopaClockUs", UintegerValue(std::max<uint32_t>(copaClockUs, 1U)));
        return;
    }
    if (cc == "copa-auto-v2" || cc == "copa_auto_v2")
    {
        const std::string path = "/NodeList/" + std::to_string(nodeId) +
            "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpCopaAutoV2/";
        Config::Set(path + "CopaClockUs", UintegerValue(std::max<uint32_t>(copaClockUs, 1U)));
        Config::Set(path + "DebugMs", UintegerValue(debugMs));
        if (flow == 0 && g_copaEst.is_open())
            Config::ConnectWithoutContext(path + "Estimates", MakeCallback(&CopaEstimateTracer));
        return;
    }
    if (!IsCopaKernelName(name))
    {
        return;
    }

    const std::string path = "/NodeList/" + std::to_string(nodeId) +
                             "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpCopaKernel/";
    const bool nosw = (name.find("+nosw") != std::string::npos);

    bool adaptive = false;
    bool modeSwitching = !nosw;
    int32_t effJitterUs = jitterUs;
    uint32_t thetaMilli = 500U;
    uint32_t initDeltaQ20 = 41943U; /* 0.04 * 2^20 */

    if (cc == "copa" || cc == "copa_orig")
    {
        adaptive = false;
        modeSwitching = !nosw;
        initDeltaQ20 = 524288U; /* 0.50 * 2^20 */
    }
    else if (cc.rfind("copa-d", 0) == 0)
    {
        const double d = std::stod(cc.substr(6));
        NS_ABORT_MSG_UNLESS(d > 0 && d <= 1, "Copa delta must be in (0, 1]: " << cc);
        adaptive = false;
        modeSwitching = !nosw;
        initDeltaQ20 = static_cast<uint32_t>(std::clamp(std::round(d * 1048576.0), 1049.0, 1048576.0));
    }
    else if (cc == "copa-auto" || cc == "copa_auto")
    {
        adaptive = true;
        modeSwitching = !nosw;
    }
    else if (cc == "copa_auto_nc")
    {
        adaptive = true;
        modeSwitching = false;
    }
    else if (cc == "copa_auto_nc_j0")
    {
        adaptive = true;
        modeSwitching = false;
        effJitterUs = 0;
    }
    else if (cc == "copa_auto_nc_j3")
    {
        adaptive = true;
        modeSwitching = false;
        effJitterUs = 3000;
    }
    else if (cc.rfind("copa_auto_theta", 0) == 0)
    {
        double th = std::stod(cc.substr(15));
        adaptive = true;
        modeSwitching = !nosw;
        thetaMilli = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(std::round(th * 1000.0)), 10, 995));
    }
    else if (cc.rfind("copa_auto_nc_theta", 0) == 0)
    {
        double th = std::stod(cc.substr(18));
        adaptive = true;
        modeSwitching = false;
        thetaMilli = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(std::round(th * 1000.0)), 10, 995));
    }

    Config::Set(path + "Adaptive", BooleanValue(adaptive));
    Config::Set(path + "ModeSwitching", BooleanValue(modeSwitching));
    Config::Set(path + "JitterUs", IntegerValue(effJitterUs));
    Config::Set(path + "ThetaMilli", UintegerValue(thetaMilli));
    Config::Set(path + "InitDeltaQ20", UintegerValue(initDeltaQ20));
    Config::Set(path + "CopaClockUs", UintegerValue(std::max<uint32_t>(copaClockUs, 1U)));
    Config::Set(path + "DebugMs", UintegerValue(debugMs));
    if (adaptive && flow == 0 && g_copaEst.is_open())
    {
        Config::ConnectWithoutContext(path + "Estimates", MakeCallback(&CopaEstimateTracer));
    }
}

/**
 * Map a short algorithm name to its ns-3 TypeId name.
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
    if (cc == "bbr2" || cc == "bbrv2" || cc == "bbrv2l")
    {
        return "ns3::TcpBbrV2";
    }
    if (cc == "bbr3" || cc == "bbrv3" || cc == "bbrv3l")
    {
        return "ns3::TcpBbrV3";
    }
    if (cc == "vegas")
    {
        return "ns3::TcpVegas";
    }
    if (cc == "veno")
    {
        return "ns3::TcpVeno";
    }
    if (cc == "yeah")
    {
        return "ns3::TcpYeah";
    }
    if (cc == "illinois")
    {
        return "ns3::TcpIllinois";
    }
    if (cc == "westwood")
    {
        return "ns3::TcpWestwoodPlus";
    }
    if (cc == "hybla")
    {
        return "ns3::TcpHybla";
    }
    if (cc == "reno" || cc == "newreno")
    {
        return "ns3::TcpLinuxReno";
    }
    if (cc == "mortise_copa")
    {
        return "ns3::TcpMortiseCopa";
    }
    if (cc == "copa-auto-v2" || cc == "copa_auto_v2")
    {
        return "ns3::TcpCopaAutoV2";
    }
    if (IsCopaKernelName(name))
    {
        return "ns3::TcpCopaKernel";
    }
    // Allow full TypeId names for algorithms added later.
    TypeId tid;
    NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(cc, &tid),
                        "Unknown congestion control '" << cc << "'");
    return cc;
}

class MortiseAppSender : public Application
{
  public:
    void Setup(Address peer, std::vector<double> arrSec, std::vector<uint32_t> sizes)
    {
        m_peer = peer;
        m_arrSec = std::move(arrSec);
        m_sizes = std::move(sizes);
    }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        m_socket->SetSendCallback(MakeCallback(&MortiseAppSender::OnSendSpace, this));
        m_socket->Connect(m_peer);
        for (size_t i = 0; i < m_arrSec.size(); i++)
        {
            Simulator::Schedule(Seconds(m_arrSec[i]), &MortiseAppSender::QueueRequest, this, m_sizes[i]);
        }
    }

    void StopApplication() override
    {
        if (m_socket)
        {
            m_socket->Close();
        }
    }

    void QueueRequest(uint32_t bytes)
    {
        m_pending += bytes;
        Pump();
    }

    void OnSendSpace(Ptr<Socket>, uint32_t)
    {
        Pump();
    }

    void Pump()
    {
        if (!m_socket)
        {
            return;
        }
        while (m_pending > 0)
        {
            uint32_t avail = m_socket->GetTxAvailable();
            if (avail == 0)
            {
                break;
            }
            uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(m_pending, std::min<uint32_t>(avail, 65536U)));
            int sent = m_socket->Send(Create<Packet>(chunk));
            if (sent <= 0)
            {
                break;
            }
            m_pending -= static_cast<uint64_t>(sent);
        }
    }

    Address m_peer;
    Ptr<Socket> m_socket;
    std::vector<double> m_arrSec;
    std::vector<uint32_t> m_sizes;
    uint64_t m_pending{0};
};

class MortiseAppReceiver : public Application
{
  public:
    std::vector<double> rctsMs;
    uint64_t totalRx{0};

    void Setup(uint16_t port, std::vector<double> arrSec, std::vector<uint32_t> sizes)
    {
        m_port = port;
        m_arrSec = std::move(arrSec);
        m_sizes = std::move(sizes);
    }

  private:
    void StartApplication() override
    {
        m_listen = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        m_listen->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
        m_listen->Listen();
        m_listen->SetAcceptCallback(MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
                                    MakeCallback(&MortiseAppReceiver::OnAccept, this));
    }

    void OnAccept(Ptr<Socket> s, const Address&)
    {
        m_conn = s;
        m_conn->SetRecvCallback(MakeCallback(&MortiseAppReceiver::OnRead, this));
    }

    void OnRead(Ptr<Socket> s)
    {
        Ptr<Packet> p;
        while ((p = s->Recv()) && p->GetSize() > 0)
        {
            totalRx += p->GetSize();
            m_accum += p->GetSize();
            double nowS = Simulator::Now().GetSeconds();
            while (m_idx < m_sizes.size() && m_accum >= m_sizes[m_idx])
            {
                m_accum -= m_sizes[m_idx];
                double rctMs = std::max((nowS - m_arrSec[m_idx]) * 1000.0, 0.1);
                rctsMs.push_back(rctMs);
                m_idx++;
            }
        }
    }

    uint16_t m_port{5000};
    Ptr<Socket> m_listen;
    Ptr<Socket> m_conn;
    std::vector<double> m_arrSec;
    std::vector<uint32_t> m_sizes;
    uint64_t m_accum{0};
    size_t m_idx{0};
};

bool
LoadWorkloadFile(const std::string& path,
                 std::vector<double>& arrSec,
                 std::vector<uint32_t>& sizes,
                 double startOffsetS)
{
    std::ifstream f(path);
    if (!f)
    {
        return false;
    }
    double tSec = startOffsetS;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::istringstream iss(line);
        double gapMs = 0.0;
        uint64_t sz = 0;
        if (!(iss >> gapMs >> sz) || sz == 0)
        {
            continue;
        }
        tSec += gapMs / 1000.0;
        arrSec.push_back(tSec);
        sizes.push_back(static_cast<uint32_t>(std::min<uint64_t>(sz, 100000000ULL)));
    }
    return !arrSec.empty();
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
        uint64_t rx = g_sinks[i] ? g_sinks[i]->GetTotalRx() : 0;
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
        g_rxAtWarmup[i] = g_sinks[i] ? g_sinks[i]->GetTotalRx() : 0;
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
            g_baseRttSamples[flow].push_back(2.0 * (g_delayMs + g_accessDelayMs));
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
    double clockGranularityMs = 1.0;
    uint32_t copaClockUs = 1;
    int32_t jitterUs = -1;
    uint32_t debugMs = 0;
    std::string workloadFile;
    double baseRttMs = 0.0;
    bool evalJson = false;

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
    cmd.AddValue("clockGranularityMs",
                 "TCP ClockGranularity in ms (default 1.0)",
                 clockGranularityMs);
    cmd.AddValue("copaClockUs",
                 "Copa timestamp/RTT quantization in us (1=us, 1000=1ms slow clock)",
                 copaClockUs);
    cmd.AddValue("jitterUs", "Override Copa-Auto jitter_us (-1=auto, >=0=manual)", jitterUs);
    cmd.AddValue("debugMs", "Emit Copa-Auto internal telemetry every N ms (0=off)", debugMs);
    cmd.AddValue("workloadFile", "Optional Mortise <gap_ms> <size_bytes> .wk workload", workloadFile);
    cmd.AddValue("baseRttMs", "Optional override total base RTT in ms (0=use bottleneckDelay/accessDelay)", baseRttMs);
    cmd.AddValue("evalJson", "Print single-line JSON compatible with mahimahi_eval.py", evalJson);
    cmd.Parse(argc, argv);

    if (baseRttMs > 0.0)
    {
        if (!delayTrace.empty())
        {
            accessDelayMs = 0.25;
            bottleneckDelayMs = 0.25;
        }
        else
        {
            accessDelayMs = 0.5;
            bottleneckDelayMs = std::max(baseRttMs / 2.0 - accessDelayMs, 0.5);
        }
    }

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
    Config::SetDefault("ns3::TcpSocketBase::ClockGranularity",
                       TimeValue(MilliSeconds(std::max(clockGranularityMs, 0.001))));
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
        g_mmDataGate->SetStatsWarmupS(workloadFile.empty() ? g_warmup : 0.0);
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
    std::vector<double> wkArrSec;
    std::vector<uint32_t> wkSizes;
    Ptr<MortiseAppReceiver> appRx;
    const bool appMode = !workloadFile.empty();
    if (appMode)
    {
        NS_ABORT_MSG_UNLESS(LoadWorkloadFile(workloadFile, wkArrSec, wkSizes, 0.2),
                            "Failed to load --workloadFile " << workloadFile);
        stopTime = std::max(stopTime, wkArrSec.back() + 25.0);
    }

    for (uint32_t i = 0; i < n; i++)
    {
        const uint16_t port = basePort + i;
        const double start = 0.1 + i * flowStagger;
        if (appMode && i == 0)
        {
            appRx = CreateObject<MortiseAppReceiver>();
            appRx->Setup(port, wkArrSec, wkSizes);
            receiver->AddApplication(appRx);
            appRx->SetStartTime(Seconds(0.0));
            appRx->SetStopTime(Seconds(stopTime));

            Ptr<MortiseAppSender> appTx = CreateObject<MortiseAppSender>();
            appTx->Setup(InetSocketAddress(bnIfaces.GetAddress(1), port), wkArrSec, wkSizes);
            senders.Get(i)->AddApplication(appTx);
            appTx->SetStartTime(Seconds(start));
            appTx->SetStopTime(Seconds(stopTime));
            g_sinks.push_back(nullptr);
        }
        else
        {
            PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                        InetSocketAddress(Ipv4Address::GetAny(), port));
            ApplicationContainer sinkApp = sinkHelper.Install(receiver);
            sinkApp.Start(Seconds(0));
            g_sinks.push_back(DynamicCast<PacketSink>(sinkApp.Get(0)));

            BulkSendHelper bulk("ns3::TcpSocketFactory",
                                InetSocketAddress(bnIfaces.GetAddress(1), port));
            bulk.SetAttribute("MaxBytes", UintegerValue(0));
            ApplicationContainer src = bulk.Install(senders.Get(i));
            src.Start(Seconds(start));
        }
        Simulator::Schedule(Seconds(start + 0.001),
                            &ConnectTraces,
                            senders.Get(i)->GetId(),
                            i);
        if (IsAdaptiveCopaName(flows[i]) && !evalJson && !prefix.empty() && !g_copaEst.is_open())
        {
            g_copaEst.open(prefix + "-copaest.data");
            g_copaEst << "# t j_ms k_ms rho r0_ms dq_target_ms delta\n";
        }
        Simulator::Schedule(Seconds(start) + MicroSeconds(1),
                            &ConfigureCcFlow,
                            senders.Get(i)->GetId(),
                            i,
                            flows[i],
                            jitterUs,
                            copaClockUs,
                            debugMs);
    }
    g_lastRx.assign(n, 0);
    g_rxAtWarmup.assign(n, 0);
    g_rttSamples.assign(n, {});
    g_baseRttSamples.assign(n, {});
    g_srttSamples.assign(n, {});
    g_binRttSum.assign(n, 0.0);
    g_binRttCount.assign(n, 0);
    g_binLastRttSum.assign(n, 0.0);
    g_binLastRttCount.assign(n, 0);
    g_retx.assign(n, 0);

    // ---- Outputs and schedule ---------------------------------------------
    if (!evalJson)
    {
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
    }

    for (const auto& seg : sched)
    {
        if (seg.start < stopTime)
        {
            Simulator::Schedule(Seconds(seg.start), &ApplySegment, seg);
        }
    }
    Simulator::Schedule(MilliSeconds(g_binMs), &SampleBin);
    Simulator::Schedule(Seconds(g_warmup), &EndWarmup);

    if (!evalJson)
    {
        std::cout << "capacity-trace: ccs=" << ccs << " profile=" << profile << " run=" << run
                  << " queue=" << queueBytes << "B aqm=" << aqm << " baseRTT="
                  << 2 * (bottleneckDelayMs + accessDelayMs) << "ms minRto=" << minRtoMs << "ms"
                  << std::endl;
    }

    Simulator::Stop(Seconds(stopTime));
    Simulator::Run();

    // ---- Summary -----------------------------------------------------------
    const double measured = std::max(stopTime - (appMode ? 0.0 : g_warmup), 1e-3);
    double sum = 0.0;
    double sumSq = 0.0;
    std::vector<double> tput(n);
    for (uint32_t i = 0; i < n; i++)
    {
        uint64_t rxBytes = (appMode && i == 0 && appRx)
                               ? appRx->totalRx
                               : (g_sinks[i] ? (g_sinks[i]->GetTotalRx() - g_rxAtWarmup[i]) : 0);
        tput[i] = rxBytes * 8.0 / measured / 1e6;
        sum += tput[i];
        sumSq += tput[i] * tput[i];
    }
    const double avgCap = g_capacityIntegral / std::max(stopTime - g_warmup, 1e-3);
    const double jain = sumSq > 0 ? sum * sum / (n * sumSq) : 0.0;

    if (evalJson)
    {
        double effBaseRttMs = (baseRttMs > 0.0) ? baseRttMs : (2.0 * (bottleneckDelayMs + accessDelayMs));
        if (profile == "file" && !g_baseRttSamples[0].empty())
        {
            double sumBase = 0.0;
            for (double base : g_baseRttSamples[0]) sumBase += base;
            effBaseRttMs = sumBase / g_baseRttSamples[0].size();
        }
        std::vector<double> qd;
        uint64_t delivPkts = 0;
        uint64_t dropPkts = 0;
        if (g_mmDataGate)
        {
            qd = g_mmDataGate->GetQDelaysMs();
            delivPkts = g_mmDataGate->GetDeliveredPkts();
            dropPkts = g_mmDataGate->GetDroppedPkts();
        }
        if (qd.empty())
        {
            const auto& rttSrc = (!g_rttSamples.empty() && !g_rttSamples[0].empty())
                                     ? g_rttSamples[0]
                                     : (!g_srttSamples.empty() ? g_srttSamples[0] : g_rttSamples[0]);
            qd.reserve(rttSrc.size());
            for (size_t i = 0; i < rttSrc.size(); ++i)
            {
                double base = profile == "file" && i < g_baseRttSamples[0].size()
                                  ? g_baseRttSamples[0][i] : effBaseRttMs;
                qd.push_back(std::max(rttSrc[i] - base, 0.0));
            }
        }
        if (delivPkts == 0 && dropPkts == 0 && !g_retx.empty())
        {
            delivPkts = static_cast<uint64_t>(std::max(sum * 1e6 / 8.0 * measured / 1448.0, 0.0));
            dropPkts = g_retx[0];
        }
        double qdAvg = 0.0;
        for (double v : qd)
        {
            qdAvg += v;
        }
        if (!qd.empty())
        {
            qdAvg /= static_cast<double>(qd.size());
        }
        double qdP95 = Percentile(qd, 95);
        double dropRate = (delivPkts + dropPkts > 0)
                              ? static_cast<double>(dropPkts) / static_cast<double>(delivPkts + dropPkts)
                              : 0.0;
        double util = (avgCap > 1e-6) ? (sum / avgCap) : 0.0;
        double estRttMs = effBaseRttMs + qdAvg;
        double utilPower = (estRttMs > 1e-6) ? (util / (estRttMs / 1000.0)) : 0.0;
        double normPower = utilPower * (effBaseRttMs / 1000.0);
        double tputPower = (estRttMs > 1e-6) ? (sum / (estRttMs / 1000.0)) : 0.0;

        std::ostringstream js;
        js << std::fixed << std::setprecision(6);
        js << "{\"duration_s\":" << measured
           << ",\"capacity_mbps\":" << avgCap
           << ",\"throughput_mbps\":" << sum
           << ",\"utilization\":" << util
           << ",\"qdelay_avg_ms\":" << qdAvg
           << ",\"qdelay_p95_ms\":" << qdP95
           << ",\"base_rtt_ms\":" << effBaseRttMs
           << ",\"est_rtt_ms\":" << estRttMs
           << ",\"util_power\":" << utilPower
           << ",\"norm_power\":" << normPower
           << ",\"tput_power\":" << tputPower
           << ",\"drop_rate\":" << dropRate;
        if (appMode && appRx)
        {
            std::vector<double> rcts = appRx->rctsMs;
            double meanRct = 0.0, p50Rct = 0.0, p95Rct = 0.0, fctSlowdown = 0.0, mortiseQoe = 0.0;
            if (!rcts.empty())
            {
                double qoeSum = 0.0, logSdSum = 0.0;
                for (size_t i = 0; i < rcts.size(); i++)
                {
                    meanRct += rcts[i];
                    uint32_t sz = (i < wkSizes.size()) ? wkSizes[i] : 1448U;
                    double rctS = std::max(rcts[i] / 1000.0, 1e-6);
                    double thrMbps = (static_cast<double>(sz) * 8.0) / (rctS * 1e6);
                    qoeSum += (thrMbps * 0.5) / std::max(rcts[i], 1.0) * 100.0;
                    double requestBase = effBaseRttMs;
                    if (profile == "file" && i < wkArrSec.size())
                    {
                        requestBase = 2.0 * (bottleneckDelayMs + accessDelayMs);
                        for (const auto& seg : sched)
                        {
                            if (seg.start > wkArrSec[i]) break;
                            if (seg.delayMs >= 0) requestBase = 2.0 * (seg.delayMs + accessDelayMs);
                        }
                    }
                    double idealMs = std::max(requestBase, 1.0) +
                                     ((avgCap > 1e-6) ? (static_cast<double>(sz) * 8.0 / (avgCap * 1000.0)) : 0.0);
                    double sd = std::max(rcts[i] / std::max(idealMs, 1e-3), 1.0);
                    logSdSum += std::log(sd);
                }
                meanRct /= static_cast<double>(rcts.size());
                mortiseQoe = qoeSum / static_cast<double>(rcts.size());
                fctSlowdown = std::exp(logSdSum / static_cast<double>(rcts.size()));
                std::vector<double> sortedRcts = rcts;
                p50Rct = Percentile(sortedRcts, 50);
                p95Rct = Percentile(sortedRcts, 95);
            }
            uint64_t retx = g_retx.empty() ? 0 : g_retx[0];
            double retxRatio = (delivPkts + retx > 0)
                                   ? static_cast<double>(retx) / static_cast<double>(delivPkts + retx)
                                   : 0.0;
            js << ",\"requests\":" << rcts.size()
               << ",\"rct_mean_ms\":" << meanRct
               << ",\"rct_p50_ms\":" << p50Rct
               << ",\"rct_p95_ms\":" << p95Rct
               << ",\"retrans_pkts\":" << retx
               << ",\"retrans_ratio\":" << retxRatio
               << ",\"fct_slowdown\":" << fctSlowdown
               << ",\"mortise_qoe\":" << mortiseQoe;
        }
        js << "}\n";
        std::cout << js.str() << std::flush;
        Simulator::Destroy();
        return 0;
    }

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
