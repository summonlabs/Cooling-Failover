// Cooling Failover - ports to the systems that actually actuate and observe.
//
// This orchestrator never touches a device. It issues typed, generation-bound
// requests to the owning control runtimes and consumes their observations.
#pragma once

#include "cooling_failover/export.hpp"
#include "cooling_failover/ids.hpp"
#include "cooling_failover/model.hpp"
#include "cooling_failover/plan.hpp"
#include "cooling_failover/status.hpp"

#include <string>
#include <vector>

namespace cooling_failover {

/// A typed request handed to an owning control runtime.
struct CF_API ControlRequest {
  CommandId command{};
  IdempotencyKey key{};
  Digest fingerprint{};
  FailoverDomainId domain{};
  PlanId plan{};
  std::uint32_t step{0};
  OwnerSystem runtime{OwnerSystem::AirflowControl};
  SourceGroupId target{};
  std::vector<EffectKind> requested_effects{};
  ControlPlaneEpoch epoch{};
  Tick issued_at{};
};

struct CF_API ControlResponse {
  CommandOutcome outcome{CommandOutcome::Acknowledged};
  std::string detail{};
};

/// Port implemented by Airflow Control / Liquid Cooling Control adapters.
class CF_API ControlRuntimePort {
 public:
  virtual ~ControlRuntimePort();

  [[nodiscard]] virtual const char* name() const noexcept = 0;

  /// Issues the request. An Acknowledged outcome is not proof that cooling
  /// service transferred; it only means the runtime accepted the request.
  [[nodiscard]] virtual Result<ControlResponse> issue(const ControlRequest& request) = 0;

  /// Returns the effect observations the runtime has recorded for \p request.
  /// Returns an empty vector when nothing has been observed yet.
  [[nodiscard]] virtual Result<std::vector<EffectObservationRecord>> poll_effects(
      const ControlRequest& request) = 0;
};

}  // namespace cooling_failover
