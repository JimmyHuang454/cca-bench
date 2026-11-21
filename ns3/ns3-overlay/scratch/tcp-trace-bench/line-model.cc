#include "line-model.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iterator>
using namespace ns3;
namespace tracebench {
std::vector<LinePoint> g_upDelay, g_downDelay, g_upCapacity;
int64_t g_linePeriodMs = 0;
double LineValue(const std::vector<LinePoint>& points, int64_t ms)
{
    auto it = std::upper_bound(points.begin(), points.end(), ms,
        [](int64_t t, const LinePoint& p) { return t < p.ms; });
    return it == points.begin() ? 0.0 : std::prev(it)->value;
}
double PeriodicLineValue(const std::vector<LinePoint>& points, int64_t ms)
{
    return LineValue(points, g_linePeriodMs > 0 ? ms % g_linePeriodMs : ms);
}
double LineIntegral(const std::vector<LinePoint>& points, double endMs, int64_t periodMs)
{
    const auto integrate = [&points](double end) {
        double total = 0;
        for (size_t i = 0; i < points.size() && points[i].ms < end; ++i)
        {
            double next = i + 1 < points.size() ? points[i + 1].ms : end;
            total += (std::min(next, end) - points[i].ms) * points[i].value;
        }
        return total;
    };
    if (periodMs <= 0) return integrate(endMs);
    double cycles = std::floor(endMs / periodMs);
    return cycles * integrate(periodMs) + integrate(endMs - cycles * periodMs);
}
std::vector<LinePoint> ReadLine(const std::string& file, bool capacity)
{
    std::ifstream in(file);
    NS_ABORT_MSG_UNLESS(in.good(), "Cannot open lines file " << file);
    std::vector<LinePoint> points;
    std::string line;
    unsigned lineNo = 0;
    while (std::getline(in, line))
    {
        ++lineNo;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream input(line);
        int64_t ms; double value; char comma;
        bool parsed = static_cast<bool>(input >> ms >> comma >> value);
        bool mbps = capacity && input.peek() == 'm';
        if (mbps) input.get();
        input >> std::ws;
        NS_ABORT_MSG_UNLESS(parsed && comma == ',' && input.eof()
            && ms >= 0 && std::isfinite(value) && value >= 0
            && (!capacity || (mbps ? value <= 12000000 : (std::floor(value) == value && value <= 1000000)))
            && (points.empty() ? ms == 0 : ms > points.back().ms),
            "Invalid lines file " << file << ":" << lineNo);
        points.push_back({ms, mbps ? value / 12.0 : value});
    }
    NS_ABORT_MSG_IF(points.empty(), "Empty lines file " << file);
    return points;
}
double BaseRttNow()
{
    int64_t t = Simulator::Now().GetMilliSeconds();
    return PeriodicLineValue(g_upDelay, t) + PeriodicLineValue(g_downDelay, t);
}

}
