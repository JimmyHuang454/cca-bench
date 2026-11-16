/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * 3-D line (railway, road or pedestrian route) for the LTE mobility scenario
 * (tcp-lte-highspeed-handover --scenario=<file>).
 *
 * The line is designed in a JSON file (scratch/lines/<name>.json, see
 * scratch/lines/README.md); scratch/preview-line.py validates it, integrates the
 * speed profile along the track and writes the resolved line read here:
 *
 *   tech 4g|5g / duration D / car_loss DB / edge_rsrp DBM / tunnel_loss DB
 *   tunnel_ramp M                     distance over which the tunnel loss builds up
 *   track x y z                       polyline, in order
 *   tunnel s0 s1                      arc-length range along the track
 *   wp t x y z                        train (UE) waypoints
 *   cell id x y z tx range hard ant az tilt bw gain ground_z
 *   tcell id x y z                    in-tunnel cell
 *   car t0 t1 DB                      car loss override over a time range
 *   itime t0 t1 DBM                   interference over a time range
 *   isrc id x y z range DBM           interference source
 *   crowd t0 t1 N MBPS x y z          N other users at (x, y, z), each
 *                                     downloading MBPS Mbit/s in [t0, t1)
 *
 * This header provides the reader (Line3d), the extra propagation loss of the
 * line (car penetration, tunnels, hard cell borders) and a 3GPP-style sector
 * antenna with downtilt.
 */

#ifndef LINE3D_H
#define LINE3D_H

#include "ns3/abort.h"
#include "ns3/angles.h"
#include "ns3/antenna-model.h"
#include "ns3/double.h"
#include "ns3/mobility-model.h"
#include "ns3/propagation-loss-model.h"
#include "ns3/vector.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ns3
{

/**
 * Resolved 3-D railway line.
 */
struct Line3d
{
    /** A cell (mast or in-tunnel cell). */
    struct Cell
    {
        std::string id;          ///< name
        Vector pos;              ///< antenna position [m]
        double txDbm{46.0};      ///< transmit power [dBm] (before soft-range resolution)
        double range{-1.0};      ///< radiation range [m], <0 = none
        bool hard{false};        ///< hard border at range
        bool sector{false};      ///< sector antenna (else omni)
        double azimuthDeg{0.0};  ///< sector azimuth (0 = +x, counter-clockwise)
        double tiltDeg{0.0};     ///< downtilt
        double beamwidthDeg{65}; ///< horizontal 3 dB beamwidth
        double gainDbi{0.0};     ///< boresight gain
        double groundZ{0.0};     ///< track height below the mast [m]
        bool tunnel{false};      ///< in-tunnel cell
    };

    /** Interference over a time range. */
    struct TimeInterference
    {
        double t0;  ///< start [s]
        double t1;  ///< end [s]
        double dbm; ///< power at the phone [dBm]
    };

    /** Interference source. */
    struct Source
    {
        std::string id; ///< name
        Vector pos;     ///< position [m]
        double range;   ///< radius [m]
        double dbm;     ///< power at the phone inside the radius [dBm]
    };

    /** Other users around the phone over a time range (dense crowd). */
    struct Crowd
    {
        double t0;   ///< start [s]
        double t1;   ///< end [s]
        uint32_t n;  ///< number of users
        double mbps; ///< downlink load of each user [Mbit/s]
        Vector pos;  ///< where the crowd stands [m]
    };

    std::string tech{"4g"};                          ///< 4g | 5g
    double duration{0.0};                            ///< simulated time [s]
    double carLoss{0.0};                             ///< default car loss [dB]
    double edgeRsrp{-110.0};                         ///< soft-range edge RSRP [dBm]
    double tunnelLoss{40.0};                         ///< tunnel wall loss [dB]
    double tunnelRamp{100.0};                        ///< portal ramp [m]
    std::vector<Vector> track;                       ///< polyline
    std::vector<std::pair<double, double>> tunnels;  ///< arc ranges [m]
    std::vector<std::pair<double, Vector>> waypoints; ///< (t, UE position)
    std::vector<Cell> cells;                         ///< masts and in-tunnel cells
    std::vector<std::pair<std::pair<double, double>, double>> carLossRanges; ///< ((t0,t1),dB)
    std::vector<TimeInterference> timeInterference;  ///< time-based interference
    std::vector<Source> sources;                     ///< interference sources
    std::vector<Crowd> crowds;                       ///< dense crowds

    /**
     * Read a resolved line file written by preview-line.py --export.
     *
     * @param path file path
     * @return the line
     */
    static Line3d Read(const std::string& path)
    {
        Line3d l;
        std::ifstream in(path);
        NS_ABORT_MSG_UNLESS(in.good(), "Cannot open resolved line " << path);
        std::string raw;
        while (std::getline(in, raw))
        {
            std::istringstream ss(raw);
            std::string key;
            if (!(ss >> key))
            {
                continue;
            }
            if (key == "tech")
            {
                ss >> l.tech;
            }
            else if (key == "duration")
            {
                ss >> l.duration;
            }
            else if (key == "car_loss")
            {
                ss >> l.carLoss;
            }
            else if (key == "edge_rsrp")
            {
                ss >> l.edgeRsrp;
            }
            else if (key == "tunnel_loss")
            {
                ss >> l.tunnelLoss;
            }
            else if (key == "tunnel_ramp")
            {
                ss >> l.tunnelRamp;
            }
            else if (key == "track")
            {
                Vector v;
                ss >> v.x >> v.y >> v.z;
                l.track.push_back(v);
            }
            else if (key == "tunnel")
            {
                double a;
                double b;
                ss >> a >> b;
                l.tunnels.emplace_back(a, b);
            }
            else if (key == "wp")
            {
                double t;
                Vector v;
                ss >> t >> v.x >> v.y >> v.z;
                l.waypoints.emplace_back(t, v);
            }
            else if (key == "cell" || key == "tcell")
            {
                Cell c;
                ss >> c.id >> c.pos.x >> c.pos.y >> c.pos.z;
                if (key == "cell")
                {
                    int hard;
                    std::string ant;
                    ss >> c.txDbm >> c.range >> hard >> ant >> c.azimuthDeg >> c.tiltDeg >>
                        c.beamwidthDeg >> c.gainDbi >> c.groundZ;
                    c.hard = hard != 0;
                    c.sector = ant == "sector";
                }
                else
                {
                    c.tunnel = true;
                    c.groundZ = c.pos.z - 6.0;
                }
                NS_ABORT_MSG_IF(ss.fail(), path << ": malformed record: " << raw);
                l.cells.push_back(c);
            }
            else if (key == "car")
            {
                double t0;
                double t1;
                double db;
                ss >> t0 >> t1 >> db;
                l.carLossRanges.push_back({{t0, t1}, db});
            }
            else if (key == "itime")
            {
                TimeInterference i;
                ss >> i.t0 >> i.t1 >> i.dbm;
                l.timeInterference.push_back(i);
            }
            else if (key == "isrc")
            {
                Source s;
                ss >> s.id >> s.pos.x >> s.pos.y >> s.pos.z >> s.range >> s.dbm;
                l.sources.push_back(s);
            }
            else if (key == "crowd")
            {
                Crowd c;
                ss >> c.t0 >> c.t1 >> c.n >> c.mbps >> c.pos.x >> c.pos.y >> c.pos.z;
                NS_ABORT_MSG_IF(ss.fail(), path << ": malformed record: " << raw);
                l.crowds.push_back(c);
            }
            else
            {
                NS_FATAL_ERROR(path << ": unknown record '" << key
                                    << "' (regenerate it with preview-line.py --export)");
            }
        }
        NS_ABORT_MSG_IF(l.track.size() < 2 || l.waypoints.empty() || l.cells.empty() ||
                            l.duration <= 0,
                        path << ": incomplete resolved line");
        return l;
    }

    /**
     * @param t time [s]
     * @return car loss at time t [dB]
     */
    double CarLossAt(double t) const
    {
        for (const auto& r : carLossRanges)
        {
            if (t >= r.first.first && t < r.first.second)
            {
                return r.second;
            }
        }
        return carLoss;
    }

    /**
     * Interference power at the phone.
     *
     * @param t time [s]
     * @param p phone position
     * @return power [dBm], -1000 when none
     */
    double InterferenceAt(double t, const Vector& p) const
    {
        double mw = 0.0;
        for (const auto& i : timeInterference)
        {
            if (t >= i.t0 && t < i.t1)
            {
                mw += std::pow(10.0, i.dbm / 10.0);
            }
        }
        for (const auto& s : sources)
        {
            if (CalculateDistance(p, s.pos) <= s.range)
            {
                mw += std::pow(10.0, s.dbm / 10.0);
            }
        }
        return mw > 0 ? 10.0 * std::log10(mw) : -1000.0;
    }

    /**
     * Project a point on the track polyline (horizontal distance).
     *
     * @param p point
     * @return (arc length, horizontal distance from the track)
     */
    std::pair<double, double> Project(const Vector& p) const
    {
        double best = std::numeric_limits<double>::infinity();
        double bestS = 0.0;
        double cum = 0.0;
        for (size_t k = 0; k + 1 < track.size(); k++)
        {
            const Vector& a = track[k];
            const Vector& b = track[k + 1];
            const double dx = b.x - a.x;
            const double dy = b.y - a.y;
            const double dz = b.z - a.z;
            const double l2 = dx * dx + dy * dy;
            double f = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.0;
            f = std::max(0.0, std::min(1.0, f));
            const double d = std::hypot(p.x - (a.x + f * dx), p.y - (a.y + f * dy));
            const double seg = std::sqrt(l2 + dz * dz);
            if (d < best)
            {
                best = d;
                bestS = cum + f * seg;
            }
            cum += seg;
        }
        return {bestS, best};
    }
};

/**
 * Extra loss of the line: car penetration (time dependent), tunnels and hard
 * cell borders.
 *
 * A node is in a tunnel when its projection on the track falls in a tunnel
 * range and it is within 10 m of the track. A link with exactly one end in a
 * tunnel gets the tunnel loss, built up over PortalRamp metres from the portal
 * (measured for the node closer to the track, the train). A link to a cell with
 * a hard border is cut (300 dB) beyond the cell's range.
 */
class Line3dLossModel : public PropagationLossModel
{
  public:
    /**
     * Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::Line3dLossModel")
                                .SetParent<PropagationLossModel>()
                                .AddConstructor<Line3dLossModel>()
                                .AddAttribute("CarLoss",
                                              "Car penetration loss [dB]",
                                              DoubleValue(0.0),
                                              MakeDoubleAccessor(&Line3dLossModel::m_carLoss),
                                              MakeDoubleChecker<double>(0.0))
                                .AddAttribute("PortalRamp",
                                              "Distance over which the tunnel loss builds up [m]",
                                              DoubleValue(100.0),
                                              MakeDoubleAccessor(&Line3dLossModel::m_ramp),
                                              MakeDoubleChecker<double>(0.0));
        return tid;
    }

    /**
     * @param line resolved line (tunnels, hard cells, tunnel loss)
     */
    void SetLine(const Line3d* line)
    {
        m_line = line;
    }

    /**
     * Apply the car (penetration) loss only to links of this node, the phone
     * under test; other users (a crowd) only see tunnels and hard borders.
     * Without it the car loss applies to every link.
     *
     * @param phone mobility model of the phone
     */
    void SetPhone(Ptr<MobilityModel> phone)
    {
        m_phone = phone;
    }

  private:
    /**
     * @param p position
     * @param s arc length of the projection (output)
     * @param lat distance from the track (output)
     * @return index of the tunnel containing p, -1 if none
     */
    int TunnelOf(const Vector& p, double& s, double& lat) const
    {
        auto pr = m_line->Project(p);
        s = pr.first;
        lat = pr.second;
        if (lat > 10.0)
        {
            return -1;
        }
        for (size_t k = 0; k < m_line->tunnels.size(); k++)
        {
            if (s >= m_line->tunnels[k].first && s <= m_line->tunnels[k].second)
            {
                return static_cast<int>(k);
            }
        }
        return -1;
    }

    double DoCalcRxPower(double txPowerDbm,
                         Ptr<MobilityModel> a,
                         Ptr<MobilityModel> b) const override
    {
        double loss = (!m_phone || a == m_phone || b == m_phone) ? m_carLoss : 0.0;
        const Vector pa = a->GetPosition();
        const Vector pb = b->GetPosition();
        if (!m_line->tunnels.empty())
        {
            double sa;
            double la;
            double sb;
            double lb;
            const int ta = TunnelOf(pa, sa, la);
            const int tb = TunnelOf(pb, sb, lb);
            if (ta != tb)
            {
                const bool aMoves = la <= lb;
                const double s = aMoves ? sa : sb;
                const int own = aMoves ? ta : tb;
                const int k = own >= 0 ? own : (aMoves ? tb : ta);
                const auto& t = m_line->tunnels[k];
                const double d = std::min(std::abs(s - t.first), std::abs(s - t.second));
                loss += m_ramp > 0 ? m_line->tunnelLoss * std::min(1.0, d / m_ramp)
                                   : m_line->tunnelLoss;
            }
        }
        for (const auto& c : m_line->cells)
        {
            if (!c.hard)
            {
                continue;
            }
            const bool aIsCell = CalculateDistance(pa, c.pos) < 0.5;
            const bool bIsCell = CalculateDistance(pb, c.pos) < 0.5;
            if ((aIsCell || bIsCell) && CalculateDistance(pa, pb) > c.range)
            {
                loss += 300.0;
                break;
            }
        }
        return txPowerDbm - loss;
    }

    int64_t DoAssignStreams(int64_t) override
    {
        return 0;
    }

    const Line3d* m_line{nullptr}; ///< the line
    Ptr<MobilityModel> m_phone;    ///< phone the car loss applies to (null = all)
    double m_carLoss{0.0};         ///< car loss [dB]
    double m_ramp{100.0};          ///< portal ramp [m]
};

NS_OBJECT_ENSURE_REGISTERED(Line3dLossModel);

/**
 * 3GPP TR 36.814 style sector antenna: horizontal and vertical parabolic
 * patterns with downtilt, 20 dB front-to-back limit.
 */
class SectorAntennaModel : public AntennaModel
{
  public:
    /**
     * Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::SectorAntennaModel")
                .SetParent<AntennaModel>()
                .AddConstructor<SectorAntennaModel>()
                .AddAttribute("Azimuth",
                              "Boresight azimuth [deg], 0 = +x, counter-clockwise",
                              DoubleValue(0.0),
                              MakeDoubleAccessor(&SectorAntennaModel::m_azimuth),
                              MakeDoubleChecker<double>())
                .AddAttribute("Tilt",
                              "Downtilt [deg], positive downwards",
                              DoubleValue(0.0),
                              MakeDoubleAccessor(&SectorAntennaModel::m_tilt),
                              MakeDoubleChecker<double>())
                .AddAttribute("HorizontalBeamwidth",
                              "Horizontal 3 dB beamwidth [deg]",
                              DoubleValue(65.0),
                              MakeDoubleAccessor(&SectorAntennaModel::m_hbw),
                              MakeDoubleChecker<double>(1.0, 360.0))
                .AddAttribute("VerticalBeamwidth",
                              "Vertical 3 dB beamwidth [deg]",
                              DoubleValue(10.0),
                              MakeDoubleAccessor(&SectorAntennaModel::m_vbw),
                              MakeDoubleChecker<double>(1.0, 180.0))
                .AddAttribute("Gain",
                              "Boresight gain [dBi]",
                              DoubleValue(15.0),
                              MakeDoubleAccessor(&SectorAntennaModel::m_gain),
                              MakeDoubleChecker<double>());
        return tid;
    }

    double GetGainDb(Angles a) override
    {
        double phi = a.GetAzimuth() * 180.0 / M_PI - m_azimuth;
        while (phi > 180.0)
        {
            phi -= 360.0;
        }
        while (phi < -180.0)
        {
            phi += 360.0;
        }
        // Inclination 0 = up, 90 = horizon; a downtilt points below the horizon.
        const double theta = a.GetInclination() * 180.0 / M_PI;
        const double ah = -std::min(12.0 * std::pow(phi / m_hbw, 2), 20.0);
        const double av = -std::min(12.0 * std::pow((theta - (90.0 + m_tilt)) / m_vbw, 2), 20.0);
        return m_gain - std::min(-(ah + av), 20.0);
    }

  private:
    double m_azimuth{0.0}; ///< boresight azimuth [deg]
    double m_tilt{0.0};    ///< downtilt [deg]
    double m_hbw{65.0};    ///< horizontal beamwidth [deg]
    double m_vbw{10.0};    ///< vertical beamwidth [deg]
    double m_gain{15.0};   ///< boresight gain [dBi]
};

NS_OBJECT_ENSURE_REGISTERED(SectorAntennaModel);

} // namespace ns3

#endif // LINE3D_H
