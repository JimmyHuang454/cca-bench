/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Extra propagation loss of a high-speed-rail line, chained after the pathloss
 * model of the radio channel (LTE or NR):
 *
 *  - car penetration loss: the UE is inside a train car, so every link between
 *    a base station and a UE is attenuated by CarLoss dB;
 *  - tunnels: a node is "in the tunnel" when its x coordinate lies within
 *    [TunnelStart, TunnelStart + TunnelLength] and it is within InTunnelWidth of
 *    the track (|y| small). Links with exactly one end in the tunnel are
 *    attenuated by TunnelLoss dB, so the outdoor cells lose the train when it
 *    enters, while the cells placed inside the tunnel (leaky feeder / distributed
 *    antennas, modelled as cells on the track) serve it. The loss grows linearly
 *    with the distance of the moving end from the portal over PortalRamp metres,
 *    which models the overlap that portal antennas provide in real deployments
 *    (without it every portal would cause a radio link failure).
 *
 * Nodes are identified as UEs/base stations only through their positions, so the
 * same model serves the LTE and the NR scenario.
 */

#ifndef TRACK_LOSS_MODEL_H
#define TRACK_LOSS_MODEL_H

#include "ns3/double.h"
#include "ns3/mobility-model.h"
#include "ns3/propagation-loss-model.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace ns3
{

/**
 * Car penetration and tunnel loss of a railway line.
 */
class TrackLossModel : public PropagationLossModel
{
  public:
    /**
     * Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::TrackLossModel")
                .SetParent<PropagationLossModel>()
                .AddConstructor<TrackLossModel>()
                .AddAttribute("CarLoss",
                              "Penetration loss of the train car [dB]",
                              DoubleValue(0.0),
                              MakeDoubleAccessor(&TrackLossModel::m_carLoss),
                              MakeDoubleChecker<double>(0.0))
                .AddAttribute("TunnelStart",
                              "x coordinate of the tunnel entrance [m]",
                              DoubleValue(0.0),
                              MakeDoubleAccessor(&TrackLossModel::m_tunnelStart),
                              MakeDoubleChecker<double>())
                .AddAttribute("TunnelLength",
                              "Tunnel length [m], 0 for no tunnel",
                              DoubleValue(0.0),
                              MakeDoubleAccessor(&TrackLossModel::m_tunnelLength),
                              MakeDoubleChecker<double>(0.0))
                .AddAttribute("TunnelLoss",
                              "Loss between a node in the tunnel and one outside it [dB]",
                              DoubleValue(40.0),
                              MakeDoubleAccessor(&TrackLossModel::m_tunnelLoss),
                              MakeDoubleChecker<double>(0.0))
                .AddAttribute("PortalRamp",
                              "Distance from the portal over which the tunnel loss "
                              "builds up [m]",
                              DoubleValue(100.0),
                              MakeDoubleAccessor(&TrackLossModel::m_ramp),
                              MakeDoubleChecker<double>(0.0))
                .AddAttribute("InTunnelWidth",
                              "Maximum |y| of a node inside the tunnel [m]",
                              DoubleValue(10.0),
                              MakeDoubleAccessor(&TrackLossModel::m_width),
                              MakeDoubleChecker<double>(0.0));
        return tid;
    }

    /**
     * Add a further tunnel (the one given by the attributes is kept).
     *
     * @param start x coordinate of the entrance [m]
     * @param length length [m]
     */
    void AddTunnel(double start, double length)
    {
        m_extraTunnels.emplace_back(start, length);
    }

    /**
     * @return all tunnels as (start, length)
     */
    std::vector<std::pair<double, double>> Tunnels() const
    {
        std::vector<std::pair<double, double>> t;
        if (m_tunnelLength > 0)
        {
            t.emplace_back(m_tunnelStart, m_tunnelLength);
        }
        t.insert(t.end(), m_extraTunnels.begin(), m_extraTunnels.end());
        return t;
    }

    /**
     * @param p position
     * @return index of the tunnel containing the position, -1 if none
     */
    int TunnelOf(const Vector& p) const
    {
        if (std::abs(p.y) > m_width)
        {
            return -1;
        }
        const auto t = Tunnels();
        for (size_t k = 0; k < t.size(); k++)
        {
            if (p.x >= t[k].first && p.x <= t[k].first + t[k].second)
            {
                return static_cast<int>(k);
            }
        }
        return -1;
    }

    /**
     * @param p position
     * @return whether the position lies inside a tunnel
     */
    bool InTunnel(const Vector& p) const
    {
        return TunnelOf(p) >= 0;
    }

  private:
    double DoCalcRxPower(double txPowerDbm,
                         Ptr<MobilityModel> a,
                         Ptr<MobilityModel> b) const override
    {
        double loss = m_carLoss;
        const Vector pa = a->GetPosition();
        const Vector pb = b->GetPosition();
        const int ta = TunnelOf(pa);
        const int tb = TunnelOf(pb);
        if (ta != tb)
        {
            // The node closer to the track is the moving one (the train); the
            // ramp is measured from the portals of the tunnel it is in, or of
            // the tunnel of the other node when it is outside.
            const bool aMoves = std::abs(pa.y) <= std::abs(pb.y);
            const Vector& m = aMoves ? pa : pb;
            const int k = (aMoves ? ta : tb) >= 0 ? (aMoves ? ta : tb) : (aMoves ? tb : ta);
            const auto t = Tunnels()[k];
            const double d = std::min(std::abs(m.x - t.first), std::abs(m.x - t.first - t.second));
            loss += m_ramp > 0 ? m_tunnelLoss * std::min(1.0, d / m_ramp) : m_tunnelLoss;
        }
        return txPowerDbm - loss;
    }

    int64_t DoAssignStreams(int64_t) override
    {
        return 0;
    }

    double m_carLoss{0.0};      ///< car penetration loss [dB]
    double m_tunnelStart{0.0};  ///< tunnel entrance [m]
    double m_tunnelLength{0.0}; ///< tunnel length [m]
    double m_tunnelLoss{40.0};  ///< loss across the tunnel wall [dB]
    double m_ramp{100.0};       ///< portal transition length [m]
    double m_width{10.0};       ///< half width of the tunnel [m]
    std::vector<std::pair<double, double>> m_extraTunnels; ///< further tunnels (start, length)
};

NS_OBJECT_ENSURE_REGISTERED(TrackLossModel);

} // namespace ns3

#endif // TRACK_LOSS_MODEL_H
