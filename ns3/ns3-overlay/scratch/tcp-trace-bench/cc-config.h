#pragma once
#include <cstdint>
#include <string>
namespace tracebench {
bool IsAdaptiveCopaName(const std::string& name);
bool IsCopaKernelName(const std::string& name);
void ConfigureCcFlow(uint32_t nodeId, uint32_t flow, std::string name,
                     int32_t jitterUs, uint32_t copaClockUs, uint32_t debugMs);
std::string CcTypeName(const std::string& name);
}
