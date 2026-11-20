#ifndef TCP_COPA_AUTO_V2_H
#define TCP_COPA_AUTO_V2_H

#include "tcp-congestion-ops.h"
#include "ns3/traced-callback.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-statement-expression-from-macro-expansion"
#endif
#include "copa_auto_v2.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace ns3
{

class TcpCopaAutoV2 : public TcpCongestionOps
{
  public:
    static TypeId GetTypeId();

    TcpCopaAutoV2();
    TcpCopaAutoV2(const TcpCopaAutoV2& other);
    ~TcpCopaAutoV2() override = default;

    std::string GetName() const override;
    bool HasCongControl() const override;
    Ptr<TcpCongestionOps> Fork() override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    void CongestionStateSet(Ptr<TcpSocketState> tcb,
                            const TcpSocketState::TcpCongState_t newState) override;
    void CongControl(Ptr<TcpSocketState> tcb,
                     const TcpRateOps::TcpRateConnection& rc,
                     const TcpRateOps::TcpRateSample& rs) override;

  private:
    void EnsureInit(Ptr<TcpSocketState> tcb);
    static uint64_t QuantizeUs(uint64_t valUs, uint32_t clockUs);

    bool m_adaptive{true};
    bool m_v2{true};
    uint32_t m_measurementRtts{16};
    bool m_modeSwitching{false};
    int32_t m_jitterUs{-1};
    uint32_t m_thetaMilli{500};
    uint32_t m_initDeltaQ20{COPA_DELTA_INIT_Q20};
    uint32_t m_copaClockUs{1};
    uint32_t m_debugMs{0};
    u64 m_nextDebugUs{0};
    bool m_initialized{false};
    struct copa_auto_state m_ca{};
    struct copa_v2_state m_v2State{};
    TracedCallback<double, double, double, double, double, double> m_estTrace;
};

} // namespace ns3

#endif // TCP_COPA_AUTO_V2_H
