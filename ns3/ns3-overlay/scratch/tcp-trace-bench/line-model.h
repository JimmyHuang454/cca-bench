#pragma once
#include "ns3/core-module.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tracebench {
struct LinePoint { int64_t ms; double value; };
struct LineScenarioData {
    int64_t endMs{0};
    std::vector<LinePoint> upCapacity;
    std::vector<LinePoint> downCapacity;
    std::vector<LinePoint> upDelay;
    std::vector<LinePoint> downDelay;
};
extern std::vector<LinePoint> g_upDelay, g_downDelay, g_upCapacity;
extern int64_t g_linePeriodMs;
double LineValue(const std::vector<LinePoint>& points, int64_t ms);
double PeriodicLineValue(const std::vector<LinePoint>& points, int64_t ms);
double LineIntegral(const std::vector<LinePoint>& points, double endMs, int64_t periodMs);
LineScenarioData ReadLineCsv(const std::string& file, double defaultDelayMs);
double BaseRttNow();
}
