#pragma once
#include "line-model.h"
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <vector>
using namespace ns3;
namespace tracebench {
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

    void SetupLines(Ptr<PointToPointNetDevice> dev, const std::string& file,
                    uint64_t limit, const std::vector<LinePoint>& delay)
    {
        m_lineMode = true;
        m_line = ReadLine(file, true);
        m_lineDelay = delay;
        m_dev = dev;
        m_limit = limit;
        m_loss = 0;
        m_uniform = CreateObject<UniformRandomVariable>();
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
        if (m_lineMode)
        {
            const double delay = PeriodicLineValue(m_lineDelay, Simulator::Now().GetMilliSeconds());
            m_lastArrival = std::max(m_lastArrival, Simulator::Now() + NanoSeconds(std::llround(delay * 1e6)));
            Simulator::Schedule(m_lastArrival - Simulator::Now(), &MahimahiGate::Enqueue, this, p->Copy());
        }
        else if (m_delays.empty())
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
        if (m_lineMode)
        {
            m_batch = static_cast<uint32_t>(PeriodicLineValue(m_line, m_tick));
            Simulator::ScheduleWithContext(m_dev->GetNode()->GetId(),
                MilliSeconds(m_tick) - Simulator::Now(), &MahimahiGate::Opportunity, this);
            ++m_tick;
            return;
        }
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

    bool m_lineMode{false};
    int64_t m_tick{0};
    std::vector<LinePoint> m_line, m_lineDelay;
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

}
