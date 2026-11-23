#include "line-model.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
using namespace ns3;
namespace tracebench {
std::vector<LinePoint> g_upDelay, g_downDelay, g_upCapacity;
int64_t g_linePeriodMs = 0;

namespace {

std::string TrimStr(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string ToLowerStr(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ParseTimeMsToken(const std::string& raw, int64_t& outMs)
{
    const std::string s = TrimStr(raw);
    if (s.empty()) return false;
    const std::string lower = ToLowerStr(s);
    std::string numStr = s;
    double mult = 1.0;
    bool hasUnit = false;
    if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "ms") == 0)
    {
        numStr = s.substr(0, s.size() - 2);
        mult = 1.0;
        hasUnit = true;
    }
    else if (lower.size() > 1 && lower.back() == 's')
    {
        numStr = s.substr(0, s.size() - 1);
        mult = 1000.0;
        hasUnit = true;
    }
    if (numStr.empty() || std::isspace(static_cast<unsigned char>(numStr.back()))) return false;
    if (!hasUnit)
    {
        if (numStr.find_first_not_of("0123456789") != std::string::npos) return false;
        std::istringstream iss(numStr);
        int64_t val = 0;
        iss >> val;
        if (iss.fail() || !iss.eof() || val < 0 || val > 1000000000000LL) return false;
        outMs = val;
        return true;
    }
    std::istringstream iss(numStr);
    double val = 0.0;
    iss >> val;
    if (iss.fail() || !iss.eof() || !std::isfinite(val) || val < 0.0) return false;
    const double scaled = val * mult;
    const double rounded = std::round(scaled);
    if (!std::isfinite(scaled) || std::fabs(scaled - rounded) > 1e-6 ||
        rounded < 0.0 || rounded > 1e12)
    {
        return false;
    }
    outMs = static_cast<int64_t>(rounded);
    return true;
}

bool ParseCapacityToken(const std::string& raw, double& outOpps)
{
    const std::string vStr = TrimStr(raw);
    if (vStr.empty()) return false;
    if (vStr == "0")
    {
        outOpps = 0.0;
        return true;
    }
    const std::string lower = ToLowerStr(vStr);
    std::string numStr;
    double mult = 0.0;
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, "gbps") == 0)
    {
        numStr = vStr.substr(0, vStr.size() - 4);
        mult = 1000.0 / 12.0;
    }
    else if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, "mbps") == 0)
    {
        numStr = vStr.substr(0, vStr.size() - 4);
        mult = 1.0 / 12.0;
    }
    else if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, "kbps") == 0)
    {
        numStr = vStr.substr(0, vStr.size() - 4);
        mult = 1.0 / 12000.0;
    }
    else if (lower.size() > 3 && lower.compare(lower.size() - 3, 3, "bps") == 0)
    {
        numStr = vStr.substr(0, vStr.size() - 3);
        mult = 1.0 / 12000000.0;
    }
    else
    {
        return false;
    }
    if (numStr.empty() || std::isspace(static_cast<unsigned char>(numStr.front())) ||
        std::isspace(static_cast<unsigned char>(numStr.back())))
        return false;
    bool hasDigit = false;
    int dotCount = 0;
    for (char ch : numStr)
    {
        if (ch >= '0' && ch <= '9')
        {
            hasDigit = true;
        }
        else if (ch == '.')
        {
            if (++dotCount > 1) return false;
        }
        else
        {
            return false;
        }
    }
    if (!hasDigit) return false;
    std::istringstream vs(numStr);
    double val = 0.0;
    vs >> val;
    if (vs.fail() || !vs.eof() || !std::isfinite(val) || val < 0.0) return false;
    const double opps = val * mult;
    if (!std::isfinite(opps) || opps > 1000000.0) return false;
    outOpps = opps;
    return true;
}

bool ParseDelayToken(const std::string& raw, double& outMs)
{
    const std::string s = TrimStr(raw);
    if (s.empty()) return false;
    const std::string lower = ToLowerStr(s);
    std::string numStr = s;
    double mult = 1.0;
    if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "ms") == 0)
    {
        numStr = s.substr(0, s.size() - 2);
        mult = 1.0;
    }
    else if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "us") == 0)
    {
        numStr = s.substr(0, s.size() - 2);
        mult = 1e-3;
    }
    else if (lower.size() > 1 && lower.back() == 's')
    {
        numStr = s.substr(0, s.size() - 1);
        mult = 1000.0;
    }
    if (numStr.empty() || std::isspace(static_cast<unsigned char>(numStr.back()))) return false;
    std::istringstream vs(numStr);
    double val = 0.0;
    vs >> val;
    if (vs.fail() || !vs.eof() || !std::isfinite(val) || val < 0.0) return false;
    const double ms = val * mult;
    if (!std::isfinite(ms)) return false;
    outMs = ms;
    return true;
}

bool ParseCsvRow(const std::string& rawLine, std::vector<std::string>& fields)
{
    fields.clear();
    std::string line = rawLine;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t i = 0;
    const size_t n = line.size();
    while (true)
    {
        std::string field;
        if (i < n && line[i] == '"')
        {
            ++i;
            bool closed = false;
            while (i < n)
            {
                if (line[i] == '"')
                {
                    if (i + 1 < n && line[i + 1] == '"')
                    {
                        field.push_back('"');
                        i += 2;
                    }
                    else
                    {
                        ++i;
                        closed = true;
                        break;
                    }
                }
                else
                {
                    field.push_back(line[i++]);
                }
            }
            if (!closed) return false;
            while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i < n && line[i] != ',') return false;
        }
        else
        {
            while (i < n && line[i] != ',')
            {
                if (line[i] == '"') return false;
                field.push_back(line[i++]);
            }
        }
        fields.push_back(std::move(field));
        if (i == n) break;
        ++i; // consume ','
    }
    return true;
}

} // namespace

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

LineScenarioData ReadLineCsv(const std::string& file, double defaultDelayMs)
{
    NS_ABORT_MSG_UNLESS(std::isfinite(defaultDelayMs) && defaultDelayMs >= 0.0,
                        "Invalid defaultDelayMs: " << defaultDelayMs);
    std::ifstream in(file);
    NS_ABORT_MSG_UNLESS(in.good(), "Cannot open lines CSV file " << file);

    static const std::set<std::string> kAllowedCols = {
        "time", "cap", "up_cap", "down_cap", "delay", "up_delay", "down_delay", "comment"
    };

    std::map<std::string, size_t> cols;
    size_t ncols = 0;
    size_t tIdx = 0;
    bool hasCapCol = false, hasUpCapCol = false, hasDownCapCol = false;
    size_t capIdx = 0, upCapIdx = 0, downCapIdx = 0;
    bool hasDelayCol = false, hasUpDelayCol = false, hasDownDelayCol = false;
    size_t delayIdx = 0, upDelayIdx = 0, downDelayIdx = 0;

    LineScenarioData out;
    bool hasUpDelay = false, hasDownDelay = false;
    bool lastUpZero = false, lastDownZero = false;
    int64_t lastRowUpDelayT = -1, lastRowDownDelayT = -1;
    int64_t prevT = -1;
    size_t rowCount = 0;

    std::string raw;
    unsigned lineNo = 0;
    std::vector<std::string> fields;
    while (std::getline(in, raw))
    {
        ++lineNo;
        if (lineNo == 1 && raw.size() >= 3 &&
            static_cast<unsigned char>(raw[0]) == 0xEF &&
            static_cast<unsigned char>(raw[1]) == 0xBB &&
            static_cast<unsigned char>(raw[2]) == 0xBF)
        {
            raw.erase(0, 3);
        }
        const std::string trimmed = TrimStr(raw);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        NS_ABORT_MSG_UNLESS(ParseCsvRow(raw, fields),
                            "Malformed CSV row in " << file << ":" << lineNo);

        if (cols.empty())
        {
            ncols = fields.size();
            for (size_t i = 0; i < ncols; ++i)
            {
                const std::string name = ToLowerStr(TrimStr(fields[i]));
                NS_ABORT_MSG_UNLESS(!name.empty() && kAllowedCols.count(name) && !cols.count(name),
                                    "Invalid or duplicate CSV column '" << fields[i] << "' in " << file << ":" << lineNo);
                cols[name] = i;
            }
            NS_ABORT_MSG_UNLESS(cols.count("time"), "Missing time column in " << file << ":" << lineNo);
            tIdx = cols["time"];
            if (cols.count("cap")) { hasCapCol = true; capIdx = cols["cap"]; }
            if (cols.count("up_cap")) { hasUpCapCol = true; upCapIdx = cols["up_cap"]; }
            if (cols.count("down_cap")) { hasDownCapCol = true; downCapIdx = cols["down_cap"]; }
            NS_ABORT_MSG_UNLESS((hasCapCol || hasUpCapCol) && (hasCapCol || hasDownCapCol),
                                "Must provide cap or both up_cap and down_cap in " << file);
            if (cols.count("delay")) { hasDelayCol = true; delayIdx = cols["delay"]; }
            if (cols.count("up_delay")) { hasUpDelayCol = true; upDelayIdx = cols["up_delay"]; }
            if (cols.count("down_delay")) { hasDownDelayCol = true; downDelayIdx = cols["down_delay"]; }
            continue;
        }

        NS_ABORT_MSG_UNLESS(fields.size() <= ncols,
                            "Too many CSV columns in " << file << ":" << lineNo);
        if (fields.size() < ncols) fields.resize(ncols);

        int64_t t = 0;
        NS_ABORT_MSG_UNLESS(ParseTimeMsToken(fields[tIdx], t),
                            "Invalid time in " << file << ":" << lineNo);
        if (rowCount == 0)
        {
            NS_ABORT_MSG_UNLESS(t == 0, "First CSV row must start at time=0 in " << file << ":" << lineNo);
        }
        else
        {
            NS_ABORT_MSG_UNLESS(t > prevT, "time must be strictly increasing in " << file << ":" << lineNo);
        }

        const std::string capRaw = hasCapCol ? TrimStr(fields[capIdx]) : std::string{};
        const std::string upCapRaw = hasUpCapCol ? TrimStr(fields[upCapIdx]) : std::string{};
        const std::string downCapRaw = hasDownCapCol ? TrimStr(fields[downCapIdx]) : std::string{};

        bool haveCap = false, haveUp = false, haveDown = false;
        double capVal = 0.0, upVal = 0.0, downVal = 0.0;
        if (!capRaw.empty())
        {
            NS_ABORT_MSG_UNLESS(ParseCapacityToken(capRaw, capVal),
                                "Invalid cap in " << file << ":" << lineNo);
            haveCap = true;
        }
        if (!upCapRaw.empty())
        {
            NS_ABORT_MSG_UNLESS(ParseCapacityToken(upCapRaw, upVal),
                                "Invalid up_cap in " << file << ":" << lineNo);
            haveUp = true;
        }
        else if (haveCap)
        {
            upVal = capVal;
            haveUp = true;
        }
        if (!downCapRaw.empty())
        {
            NS_ABORT_MSG_UNLESS(ParseCapacityToken(downCapRaw, downVal),
                                "Invalid down_cap in " << file << ":" << lineNo);
            haveDown = true;
        }
        else if (haveCap)
        {
            downVal = capVal;
            haveDown = true;
        }

        if (rowCount == 0)
        {
            NS_ABORT_MSG_UNLESS(haveUp && haveDown,
                                "Initial t=0 capacity required in " << file << ":" << lineNo);
        }
        if (haveUp)
        {
            if (out.upCapacity.empty() || upVal != out.upCapacity.back().value)
            {
                out.upCapacity.push_back({t, upVal});
            }
            lastUpZero = (upVal == 0.0);
        }
        else
        {
            lastUpZero = false;
        }
        if (haveDown)
        {
            if (out.downCapacity.empty() || downVal != out.downCapacity.back().value)
            {
                out.downCapacity.push_back({t, downVal});
            }
            lastDownZero = (downVal == 0.0);
        }
        else
        {
            lastDownZero = false;
        }

        const std::string delayRaw = hasDelayCol ? TrimStr(fields[delayIdx]) : std::string{};
        const std::string upDelayRaw = hasUpDelayCol ? TrimStr(fields[upDelayIdx]) : std::string{};
        const std::string downDelayRaw = hasDownDelayCol ? TrimStr(fields[downDelayIdx]) : std::string{};

        bool haveDelay = false, haveUpD = false, haveDownD = false;
        double commonD = 0.0, upDVal = 0.0, downDVal = 0.0;
        if (!delayRaw.empty())
        {
            NS_ABORT_MSG_UNLESS(ParseDelayToken(delayRaw, commonD),
                                "Invalid delay in " << file << ":" << lineNo);
            haveDelay = true;
        }
        if (!upDelayRaw.empty())
        {
            NS_ABORT_MSG_UNLESS(ParseDelayToken(upDelayRaw, upDVal),
                                "Invalid up_delay in " << file << ":" << lineNo);
            haveUpD = true;
        }
        else if (haveDelay)
        {
            upDVal = commonD;
            haveUpD = true;
        }
        if (!downDelayRaw.empty())
        {
            NS_ABORT_MSG_UNLESS(ParseDelayToken(downDelayRaw, downDVal),
                                "Invalid down_delay in " << file << ":" << lineNo);
            haveDownD = true;
        }
        else if (haveDelay)
        {
            downDVal = commonD;
            haveDownD = true;
        }

        if (rowCount == 0)
        {
            if (haveUpD)
            {
                hasUpDelay = true;
                out.upDelay.push_back({t, upDVal});
            }
            if (haveDownD)
            {
                hasDownDelay = true;
                out.downDelay.push_back({t, downDVal});
            }
        }
        else
        {
            if (haveUpD)
            {
                NS_ABORT_MSG_UNLESS(hasUpDelay, "up delay missing at t=0 in " << file << ":" << lineNo);
                if (out.upDelay.empty() || upDVal != out.upDelay.back().value)
                {
                    out.upDelay.push_back({t, upDVal});
                    lastRowUpDelayT = t;
                }
                else
                {
                    lastRowUpDelayT = -1;
                }
            }
            else
            {
                lastRowUpDelayT = -1;
            }
            if (haveDownD)
            {
                NS_ABORT_MSG_UNLESS(hasDownDelay, "down delay missing at t=0 in " << file << ":" << lineNo);
                if (out.downDelay.empty() || downDVal != out.downDelay.back().value)
                {
                    out.downDelay.push_back({t, downDVal});
                    lastRowDownDelayT = t;
                }
                else
                {
                    lastRowDownDelayT = -1;
                }
            }
            else
            {
                lastRowDownDelayT = -1;
            }
        }

        prevT = t;
        ++rowCount;
    }

    NS_ABORT_MSG_UNLESS(!cols.empty() && rowCount >= 2 && prevT > 0,
                        "CSV lines file must contain header and at least 2 rows ending at end_ms > 0: " << file);
    NS_ABORT_MSG_UNLESS(lastUpZero && lastDownZero,
                        "Final row in CSV lines file must explicitly set capacity to 0: " << file);

    out.endMs = prevT;
    if (out.upCapacity.back().ms != out.endMs) out.upCapacity.push_back({out.endMs, 0.0});
    if (out.downCapacity.back().ms != out.endMs) out.downCapacity.push_back({out.endMs, 0.0});

    if (hasUpDelay && lastRowUpDelayT == out.endMs && !out.upDelay.empty() && out.upDelay.back().ms == out.endMs)
    {
        out.upDelay.pop_back();
    }
    if (hasDownDelay && lastRowDownDelayT == out.endMs && !out.downDelay.empty() && out.downDelay.back().ms == out.endMs)
    {
        out.downDelay.pop_back();
    }

    if (!hasUpDelay) out.upDelay = {{0, defaultDelayMs}};
    if (!hasDownDelay) out.downDelay = {{0, defaultDelayMs}};
    return out;
}

double BaseRttNow()
{
    int64_t t = Simulator::Now().GetMilliSeconds();
    return PeriodicLineValue(g_upDelay, t) + PeriodicLineValue(g_downDelay, t);
}

} // namespace tracebench
