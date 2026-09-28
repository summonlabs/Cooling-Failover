// Cooling Failover - completed-operation benchmark.
//
// The measured workload is a complete, durable failover orchestration: create a
// plan, issue every command to the owning control runtimes, verify every
// required effect, and commit every mutation to the write-ahead log with an
// explicit flush and read-back verification. Reported latency therefore includes
// the durability cost.
//
// REAL: process, filesystem, flush and durability behaviour on this host.
// SYNTHETIC: the cooling plant, its sensors and its control runtimes.
#include "cooling_failover/orchestrator.hpp"
#include "cooling_failover/synthetic.hpp"
#include "cooling_failover/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace cf = cooling_failover;
using namespace cooling_failover::synthetic;

namespace {

struct Sample {
  double milliseconds{0.0};
};

double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const std::size_t index = static_cast<std::size_t>(
      std::min<double>(fraction * static_cast<double>(values.size() - 1),
                       static_cast<double>(values.size() - 1)));
  return values[index];
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t iterations = 200;
  if (argc > 1) {
    iterations = std::strtoull(argv[1], nullptr, 10);
  }
  const std::filesystem::path root =
      argc > 2 ? std::filesystem::path(argv[2])
               : std::filesystem::temp_directory_path() / "cooling-failover-bench";
  std::filesystem::remove_all(root);

  std::cout << "cooling-failover benchmark " << cf::version_string() << " ("
            << cf::build_identity() << ")\n";
  std::cout << "workload: complete durable failover (plan + issue + observe + verify + commit)\n";
  std::cout << "durability: flush and read-back verification on every commit\n";
  std::cout << "evidence: REAL process/filesystem/durability, SYNTHETIC plant and runtimes\n";
  std::cout << "iterations: " << iterations << " (plus a 20% warm-up that is not reported)\n\n";

  const cf::FailoverDomainId domain = cf::FailoverDomainId::from_value(0xBEEF0001);
  const cf::ControlPlaneEpoch epoch = cf::ControlPlaneEpoch::from_value(1);
  SyntheticFacility facility(two_group_plant(domain));
  SyntheticControlRuntime airflow(cf::OwnerSystem::AirflowControl, "bench-airflow");
  SyntheticControlRuntime liquid(cf::OwnerSystem::LiquidCoolingControl, "bench-liquid");

  cf::OrchestratorOptions options;
  options.store.root = root;
  auto opened = cf::CoolingFailoverOrchestrator::open(options, domain);
  if (!opened.has_value()) {
    std::cerr << "error: " << opened.status().canonical_line() << "\n";
    return 1;
  }
  std::unique_ptr<cf::CoolingFailoverOrchestrator> orchestrator = std::move(*opened);
  orchestrator->set_control_runtime(cf::OwnerSystem::AirflowControl, &airflow);
  orchestrator->set_control_runtime(cf::OwnerSystem::LiquidCoolingControl, &liquid);

  cf::Tick now = cf::Tick::from_value(0);
  std::uint64_t next_evidence = 100000;

  const auto bootstrap = [&]() -> bool {
    if (!orchestrator->register_domain(now).has_value()) {
      return false;
    }
    if (!orchestrator->import_topology(facility.topology(cf::EvidenceId::from_value(++next_evidence), now))
             .has_value()) {
      return false;
    }
    if (!orchestrator->record_capacity(facility.capacity(cf::EvidenceId::from_value(++next_evidence), now))
             .has_value()) {
      return false;
    }
    if (!orchestrator->install_policy(facility.spec().policy).has_value()) {
      return false;
    }
    if (!orchestrator->install_obligations(
             facility.obligations(cf::EvidenceId::from_value(++next_evidence), now))
             .has_value()) {
      return false;
    }
    if (!orchestrator->record_authority(
             facility.authority(cf::EvidenceId::from_value(++next_evidence), now))
             .has_value()) {
      return false;
    }
    for (const SyntheticGroup& group : facility.spec().groups) {
      if (!orchestrator->record_health(
              facility.health_for(group.source, cf::EvidenceId::from_value(++next_evidence), now))
               .has_value()) {
        return false;
      }
    }
    return orchestrator->establish_epoch(epoch, now).has_value();
  };

  if (!bootstrap()) {
    std::cerr << "error: synthetic bootstrap failed\n";
    return 1;
  }

  // Alternates between two dispositions so that every iteration performs a real
  // failover and a real return-to-primary.
  std::uint64_t request_counter = 0;
  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  std::uint64_t committed_records = 0;

  const auto one_operation = [&](bool primary_healthy) -> bool {
    now = cf::Tick::from_value(now.value() + 10);
    request_counter += 1;
    facility.set_health(cf::CoolingSourceId::from_value(1),
                        primary_healthy ? cf::HealthState::Healthy : cf::HealthState::Unavailable);
    for (const SyntheticGroup& group : facility.spec().groups) {
      if (!orchestrator
               ->record_health(facility.health_for(group.source, cf::EvidenceId::from_value(++next_evidence),
                                                   now))
               .has_value()) {
        return false;
      }
    }
    cf::PlanRequest request;
    request.request = cf::RequestId::from_value(request_counter);
    request.now = now;
    request.kind = primary_healthy ? cf::PlanKind::ReturnToPrimary : cf::PlanKind::Failover;
    request.incumbent = cf::SourceGroupId::from_value(primary_healthy ? 2 : 1);
    request.reason = "benchmark iteration";
    auto plan = orchestrator->create_plan(request);
    if (!plan.has_value()) {
      return false;
    }
    auto attempt = orchestrator->execute_plan(plan->id, now);
    if (!attempt.has_value()) {
      return false;
    }
    return attempt->state == cf::AttemptState::Verified;
  };

  const std::uint64_t warmup = std::max<std::uint64_t>(1, iterations / 5);
  for (std::uint64_t index = 0; index < warmup; ++index) {
    if (!one_operation(index % 2 == 0)) {
      std::cerr << "error: warm-up iteration failed\n";
      return 1;
    }
  }

  std::vector<double> latencies;
  latencies.reserve(static_cast<std::size_t>(iterations));
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t previous_commit = orchestrator->state().last_commit.value();
  for (std::uint64_t index = 0; index < iterations; ++index) {
    const auto operation_start = std::chrono::steady_clock::now();
    const bool ok = one_operation(index % 2 == 0);
    const auto operation_end = std::chrono::steady_clock::now();
    const std::uint64_t commit = orchestrator->state().last_commit.value();
    committed_records += commit - previous_commit;
    previous_commit = commit;
    if (!ok) {
      ++failed;
      continue;
    }
    ++completed;
    latencies.push_back(
        std::chrono::duration<double, std::milli>(operation_end - operation_start).count());
  }
  const auto end = std::chrono::steady_clock::now();
  const double total_seconds = std::chrono::duration<double>(end - start).count();

  std::cout << "completed durable failovers: " << completed << "\n";
  std::cout << "failed operations: " << failed << "\n";
  std::cout << "durable records committed: " << committed_records << "\n";
  std::cout << "elapsed: " << total_seconds << " s\n";
  if (completed > 0 && total_seconds > 0.0) {
    std::cout << "throughput: " << (static_cast<double>(completed) / total_seconds)
              << " completed failovers/s\n";
  }
  if (!latencies.empty()) {
    std::cout << "latency p50: " << percentile(latencies, 0.50) << " ms\n";
    std::cout << "latency p90: " << percentile(latencies, 0.90) << " ms\n";
    std::cout << "latency p99: " << percentile(latencies, 0.99) << " ms\n";
    std::cout << "latency max: " << percentile(latencies, 1.0) << " ms\n";
  }
  double sum = 0.0;
  for (double value : latencies) {
    sum += value;
  }
  if (!latencies.empty()) {
    std::cout << "latency mean: " << (sum / static_cast<double>(latencies.size())) << " ms\n";
  }

  if (auto closed = orchestrator->close(); !closed.has_value()) {
    std::cerr << "error: " << closed.status().canonical_line() << "\n";
    return 1;
  }
  return failed == 0 ? 0 : 1;
}
