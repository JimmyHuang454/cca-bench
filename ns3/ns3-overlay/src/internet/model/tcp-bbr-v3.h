/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * BBRv3-style congestion control.
 *
 * This is a thin refinement of TcpBbrV2 that emulates the two mechanisms that
 * define BBRv3 relative to BBRv2:
 *
 *  - a bw_lo minimum-bandwidth filter that bounds the pacing rate during loss
 *    recovery, so a stale inflight_lo cannot keep the rate too high;
 *  - a two-phase ProbeBW cycle (PROBE_UP then PROBE_DOWN) instead of the
 *    four-phase UP/DOWN/CRUISE/REFILL cycle.
 *
 * Like TcpBbrV2 this is a mechanism-level model, not a line-by-line port of
 * the Linux BBRv3 implementation.
 */

#ifndef TCP_BBR_V3_H
#define TCP_BBR_V3_H

#include "tcp-bbr-v2.h"

namespace ns3
{

/**
 * @ingroup congestionOps
 *
 * @brief BBRv3-style congestion control algorithm.
 */
class TcpBbrV3 : public TcpBbrV2
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    TcpBbrV3();

    /**
     * @brief Copy constructor.
     * @param sock object to copy.
     */
    TcpBbrV3(const TcpBbrV3& sock);

    ~TcpBbrV3() override;

    std::string GetName() const override;
    Ptr<TcpCongestionOps> Fork() override;
};

} // namespace ns3

#endif // TCP_BBR_V3_H
