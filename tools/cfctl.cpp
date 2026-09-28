// cfctl - Cooling Failover administration and inspection CLI.
//
// The facility driven by this CLI is SYNTHETIC. It exists to exercise and
// inspect the orchestration semantics; it is not a connection to real cooling
// hardware.
#include "cooling_failover/orchestrator.hpp"
#include "cooling_failover/synthetic.hpp"
#include "cooling_failover/version.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace cf = cooling_failover;
using namespace cooling_failover::synthetic;

namespace {

struct Options {
  std::string command;
  std::filesystem::path root;
  cf::FailoverDomainId domain{cf::FailoverDomainId::from_value(0xDCCF0001)};
  std::uint64_t incumbent{1};
  std::uint64_t target{0};
  std::uint64_t attempt{0};
  bool json{false};
  std::vector<std::uint64_t> degrade{};
  std::vector<std::uint64_t> heal{};
};

int usage() {
  std::cout <<
      "cfctl - Cooling Failover administration (SYNTHETIC facility)\n"
      "\n"
      "usage: cfctl <command> [options]\n"
      "\n"
      "commands:\n"
      "  version                     print version and build identity\n"
      "  init                        create the store and install the synthetic domain\n"
      "  status                      print the domain status snapshot\n"
      "  candidates                  evaluate and print the candidate arrangements\n"
      "  failover                    create, execute and verify a failover plan\n"
      "  recover                     evaluate return-to-primary eligibility\n"
      "  checkpoint                  publish a snapshot and start a fresh log\n"
      "  digest                      print the canonical state digest\n"
      "\n"
      "options:\n"
      "  --root DIR                  store root directory (required)\n"
      "  --domain N                  numeric failover domain identity (default 1)\n"
      "  --incumbent N               incumbent source group identity (default 1)\n"
      "  --target N                  explicit target source group identity\n"
      "  --attempt N                 attempt identity for verify\n"
      "  --degrade N                 mark source group N unavailable before planning\n"
      "  --heal N                    mark source group N healthy before planning\n"
      "  --json                      emit machine-readable output\n";
  return 2;
}

bool parse_options(int argc, char** argv, Options& options) {
  if (argc < 2) {
    return false;
  }
  options.command = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&](std::string& out) {
      if (index + 1 >= argc) {
        return false;
      }
      out = argv[++index];
      return true;
    };
    std::string value;
    if (argument == "--json") {
      options.json = true;
    } else if (argument == "--root") {
      if (!next(value)) {
        return false;
      }
      options.root = value;
    } else if (argument == "--domain") {
      if (!next(value)) {
        return false;
      }
      options.domain = cf::FailoverDomainId::from_value(std::strtoull(value.c_str(), nullptr, 10));
    } else if (argument == "--incumbent") {
      if (!next(value)) {
        return false;
      }
      options.incumbent = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--target") {
      if (!next(value)) {
        return false;
      }
      options.target = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--attempt") {
      if (!next(value)) {
        return false;
      }
      options.attempt = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--degrade") {
      if (!next(value)) {
        return false;
      }
      options.degrade.push_back(std::strtoull(value.c_str(), nullptr, 10));
    } else if (argument == "--heal") {
      if (!next(value)) {
        return false;
      }
      options.heal.push_back(std::strtoull(value.c_str(), nullptr, 10));
    } else {
      std::cerr << "unknown option: " << argument << "\n";
      return false;
    }
  }
  return true;
}

int fail(const cf::Status& status) {
  std::cerr << "error: " << status.canonical_line() << "\n";
  switch (status.error_class()) {
    case cf::ErrorClass::None:
      return 0;
    case cf::ErrorClass::Invalid:
      return 3;
    case cf::ErrorClass::Stale:
      return 4;
    case cf::ErrorClass::Denied:
      return 5;
    case cf::ErrorClass::Indeterminate:
      return 6;
    case cf::ErrorClass::Conflict:
      return 7;
    case cf::ErrorClass::Unavailable:
      return 8;
    case cf::ErrorClass::Unsupported:
      return 9;
    case cf::ErrorClass::Internal:
      return 10;
  }
  return 1;
}

/// Deterministic identity of the synthetic source that owns a source group.
cf::CoolingSourceId source_of(const SyntheticSpec& spec, std::uint64_t group) {
  for (const SyntheticGroup& entry : spec.groups) {
    if (entry.group.value() == group) {
      return entry.source;
    }
  }
  return cf::CoolingSourceId::nil();
}

struct Session {
  SyntheticFacility facility;
  SyntheticControlRuntime airflow;
  SyntheticControlRuntime liquid;
  std::unique_ptr<cf::CoolingFailoverOrchestrator> orchestrator;
  std::uint64_t next_evidence{5000};
  cf::Tick now{};

  explicit Session(cf::FailoverDomainId domain)
      : facility(two_group_plant(domain)),
        airflow(cf::OwnerSystem::AirflowControl, "cfctl-airflow"),
        liquid(cf::OwnerSystem::LiquidCoolingControl, "cfctl-liquid") {}

  [[nodiscard]] cf::Result<void> open(const std::filesystem::path& root) {
    cf::OrchestratorOptions options;
    options.store.root = root;
    CF_TRY_ASSIGN(opened, cf::CoolingFailoverOrchestrator::open(options, facility.domain()));
    opened->set_control_runtime(cf::OwnerSystem::AirflowControl, &airflow);
    opened->set_control_runtime(cf::OwnerSystem::LiquidCoolingControl, &liquid);
    orchestrator = std::move(opened);
    // Resume the logical clock from durable state. Evidence recorded by an
    // earlier invocation is in the past, never in the future.
    now = orchestrator->state().last_tick;
    // Evidence identities are derived from the durable commit sequence so that
    // repeated invocations never reuse an identity for different evidence.
    next_evidence = orchestrator->state().last_commit.value() * 4096ULL + 5000ULL;
    return cf::Result<void>();
  }

  [[nodiscard]] cf::EvidenceId evidence() { return cf::EvidenceId::from_value(++next_evidence); }

  [[nodiscard]] cf::Result<void> install_domain() {
    CF_TRY(orchestrator->register_domain(now));
    CF_TRY(orchestrator->import_topology(facility.topology(evidence(), now)));
    CF_TRY(orchestrator->record_capacity(facility.capacity(evidence(), now)));
    CF_TRY(orchestrator->install_policy(facility.spec().policy));
    CF_TRY(orchestrator->install_obligations(facility.obligations(evidence(), now)));
    CF_TRY(orchestrator->record_authority(facility.authority(evidence(), now)));
    for (const SyntheticGroup& group : facility.spec().groups) {
      CF_TRY(orchestrator->record_health(facility.health_for(group.source, evidence(), now)));
    }
    CF_TRY(orchestrator->establish_epoch(cf::ControlPlaneEpoch::from_value(1), now));
    return cf::Result<void>();
  }

  [[nodiscard]] cf::Result<void> apply_health_changes(const Options& options) {
    bool changed = false;
    for (std::uint64_t group : options.degrade) {
      const cf::CoolingSourceId source = source_of(facility.spec(), group);
      if (source.is_nil()) {
        return cf::Status::error(cf::ErrorCode::InvalidArgument,
                                 "degrade target is not a source group in the topology", "group",
                                 group);
      }
      facility.set_health(source, cf::HealthState::Unavailable);
      changed = true;
    }
    for (std::uint64_t group : options.heal) {
      const cf::CoolingSourceId source = source_of(facility.spec(), group);
      if (source.is_nil()) {
        return cf::Status::error(cf::ErrorCode::InvalidArgument,
                                 "heal target is not a source group in the topology", "group",
                                 group);
      }
      facility.set_health(source, cf::HealthState::Healthy);
      changed = true;
    }
    if (!changed) {
      return cf::Result<void>();
    }
    now = cf::Tick::from_value(now.value() + 1);
    for (const SyntheticGroup& group : facility.spec().groups) {
      CF_TRY(orchestrator->record_health(facility.health_for(group.source, evidence(), now)));
    }
    return cf::Result<void>();
  }
};

void print_status(const cf::DomainStatus& status, bool json) {
  if (json) {
    std::cout << "{\"domain\":\"" << status.domain.to_string() << "\",\"revision\":"
              << status.revision.value() << ",\"commit\":" << status.commit.value()
              << ",\"epoch\":" << status.epoch.value()
              << ",\"epoch_established\":" << (status.epoch_established ? "true" : "false")
              << ",\"authority\":\"" << cf::to_string(status.authority) << "\""
              << ",\"topology\":" << status.topology.value()
              << ",\"capacity\":" << status.capacity.value()
              << ",\"policy\":" << status.policy.value()
              << ",\"groups\":" << status.group_count << ",\"sources\":" << status.source_count
              << ",\"blocks\":" << status.block_count << ",\"plans\":" << status.plan_count
              << ",\"attempts\":" << status.attempt_count
              << ",\"observations\":" << status.observation_count
              << ",\"degraded\":" << (status.degraded ? "true" : "false")
              << ",\"state_digest\":\"" << status.state_digest.to_hex() << "\"}\n";
    return;
  }
  std::cout << "domain            " << status.domain.to_string() << "\n";
  std::cout << "revision          " << status.revision.value() << "\n";
  std::cout << "commit sequence   " << status.commit.value() << "\n";
  std::cout << "epoch             " << status.epoch.value()
            << (status.epoch_established ? " (established)" : " (not established)") << "\n";
  std::cout << "authority         " << cf::to_string(status.authority) << "\n";
  std::cout << "generations       topology=" << status.topology.value()
            << " capacity=" << status.capacity.value() << " policy=" << status.policy.value()
            << "\n";
  std::cout << "topology          groups=" << status.group_count
            << " sources=" << status.source_count << " blocks=" << status.block_count
            << (status.has_topology ? "" : " (absent)") << "\n";
  std::cout << "capacity evidence " << (status.has_capacity ? "current" : "absent") << "\n";
  std::cout << "policy            " << (status.has_policy ? "installed" : "absent") << "\n";
  std::cout << "obligations       " << (status.has_obligations ? "installed" : "absent") << "\n";
  std::cout << "plans             " << status.plan_count << "\n";
  std::cout << "attempts          " << status.attempt_count << "\n";
  std::cout << "observations      " << status.observation_count << "\n";
  std::cout << "active plan       "
            << (status.active_plan.is_nil() ? std::string("<none>") : status.active_plan.to_string())
            << "\n";
  std::cout << "active attempt    "
            << (status.active_attempt.is_nil() ? std::string("<none>")
                                               : status.active_attempt.to_string())
            << " " << cf::to_string(status.active_attempt_state) << "\n";
  std::cout << "degraded          " << (status.degraded ? "true" : "false") << "\n";
  std::cout << "recovered         " << (status.recovered ? "true" : "false") << "\n";
  std::cout << "state digest      " << status.state_digest.to_hex() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    return usage();
  }
  if (options.command == "version") {
    std::cout << cf::version_string() << " " << cf::build_identity() << "\n";
    return 0;
  }
  if (options.root.empty() && options.command != "help") {
    std::cerr << "error: --root is required\n";
    return usage();
  }

  Session session(options.domain);
  if (auto status = session.open(options.root); !status.has_value()) {
    return fail(status.status());
  }

  const bool already_initialized = session.orchestrator->state().registered;
  if (options.command == "init") {
    if (!already_initialized) {
      if (auto status = session.install_domain(); !status.has_value()) {
        return fail(status.status());
      }
    }
  } else if (options.command != "status" && options.command != "candidates" &&
             options.command != "failover" && options.command != "recover" &&
             options.command != "checkpoint" && options.command != "digest") {
    return usage();
  }

  if (options.command == "init") {
    if (already_initialized) {
      std::cout << "domain already initialized; installed evidence is unchanged\n";
    }
    auto status = session.orchestrator->status();
    if (!status.has_value()) {
      return fail(status.status());
    }
    print_status(*status, options.json);
    return 0;
  }

  if (options.command == "status") {
    auto status = session.orchestrator->status();
    if (!status.has_value()) {
      return fail(status.status());
    }
    print_status(*status, options.json);
    return 0;
  }

  if (options.command == "digest") {
    std::cout << session.orchestrator->state_digest().to_hex() << "\n";
    return 0;
  }

  if (options.command == "checkpoint") {
    if (auto status = session.orchestrator->checkpoint(); !status.has_value()) {
      return fail(status.status());
    }
    std::cout << "snapshot published at commit "
              << session.orchestrator->state().last_commit.value() << "\n";
    return 0;
  }

  if (options.command == "candidates") {
    if (auto applied = session.apply_health_changes(options); !applied.has_value()) {
      return fail(applied.status());
    }
    auto set = session.orchestrator->evaluate_candidates(session.now);
    if (!set.has_value()) {
      return fail(set.status());
    }
    if (options.json) {
      std::cout << "{\"candidates\":[";
      bool first = true;
      for (const cf::FailoverCandidate& candidate : set->candidates) {
        if (!first) {
          std::cout << ",";
        }
        first = false;
        std::cout << "{\"id\":\"" << candidate.id.to_string() << "\",\"groups\":[";
        for (std::size_t index = 0; index < candidate.key.groups.size(); ++index) {
          if (index != 0) {
            std::cout << ",";
          }
          std::cout << "\"" << candidate.key.groups[index].to_string() << "\"";
        }
        std::cout << "],\"usable_watts\":" << candidate.usable_capacity.value()
                  << ",\"state\":\"" << cf::to_string(candidate.state) << "\",\"rank\":"
                  << (candidate.rank == UINT32_MAX ? -1 : static_cast<int>(candidate.rank))
                  << ",\"causes\":[";
        for (std::size_t index = 0; index < candidate.causes.size(); ++index) {
          if (index != 0) {
            std::cout << ",";
          }
          std::cout << "\"" << cf::to_string(candidate.causes[index]) << "\"";
        }
        std::cout << "]}";
      }
      std::cout << "],\"selected\":\""
                << (set->selected.is_nil() ? std::string("") : set->selected.to_string())
                << "\"}\n";
      return 0;
    }
    for (const cf::FailoverCandidate& candidate : set->candidates) {
      std::cout << candidate.id.to_string() << "  groups=[";
      for (std::size_t index = 0; index < candidate.key.groups.size(); ++index) {
        if (index != 0) {
          std::cout << ",";
        }
        std::cout << candidate.key.groups[index].value();
      }
      std::cout << "]  usable=" << candidate.usable_capacity.to_string()
                << "  state=" << cf::to_string(candidate.state);
      if (candidate.rank != UINT32_MAX) {
        std::cout << "  rank=" << candidate.rank;
      }
      if (!candidate.causes.empty()) {
        std::cout << "  causes=";
        for (std::size_t index = 0; index < candidate.causes.size(); ++index) {
          if (index != 0) {
            std::cout << ",";
          }
          std::cout << cf::to_string(candidate.causes[index]);
        }
      }
      std::cout << "\n";
    }
    std::cout << "selected: "
              << (set->selected.is_nil() ? std::string("<none>") : set->selected.to_string())
              << "\n";
    return 0;
  }

  if (options.command == "recover") {
    if (auto applied = session.apply_health_changes(options); !applied.has_value()) {
      return fail(applied.status());
    }
    auto decision = session.orchestrator->evaluate_recovery(session.now);
    if (!decision.has_value()) {
      return fail(decision.status());
    }
    std::cout << "eligible            " << (decision->eligible ? "true" : "false") << "\n";
    std::cout << "primary cause       " << cf::to_string(decision->primary_cause) << "\n";
    std::cout << "incumbent           " << decision->incumbent.value() << "\n";
    std::cout << "target              " << decision->target.value() << "\n";
    std::cout << "healthy streak      " << decision->consecutive_healthy << "\n";
    std::cout << "dwell ticks         " << decision->dwell_ticks << "\n";
    std::cout << "transitions in window " << decision->transitions_in_window << "\n";
    std::cout << "detail              " << decision->detail << "\n";
    return decision->eligible ? 0 : 6;
  }

  if (options.command == "failover") {
    if (auto applied = session.apply_health_changes(options); !applied.has_value()) {
      return fail(applied.status());
    }
    cf::PlanRequest request;
    request.request = cf::RequestId::from_value(1);
    request.now = session.now;
    request.kind = cf::PlanKind::Failover;
    request.incumbent = cf::SourceGroupId::from_value(options.incumbent);
    request.target = cf::SourceGroupId::from_value(options.target);
    request.reason = "cfctl failover";
    auto plan = session.orchestrator->create_plan(request);
    if (!plan.has_value()) {
      return fail(plan.status());
    }
    std::cout << "plan " << plan->id.to_string() << " target " << plan->target.to_string()
              << " commands " << plan->commands.size() << "\n";
    auto attempt = session.orchestrator->execute_plan(plan->id, session.now);
    if (!attempt.has_value()) {
      std::cout << "attempt state " << cf::to_string(cf::AttemptState::Fenced) << "\n";
      return fail(attempt.status());
    }
    std::cout << "attempt " << attempt->id.to_string() << " state "
              << cf::to_string(attempt->state) << "\n";
    std::cout << "verified:";
    for (cf::EffectKind effect : attempt->verified_effects) {
      std::cout << " " << cf::to_string(effect);
    }
    std::cout << "\nmissing:";
    for (cf::EffectKind effect : attempt->missing_effects) {
      std::cout << " " << cf::to_string(effect);
    }
    std::cout << "\n";
    if (attempt->state != cf::AttemptState::Verified) {
      return 6;  // partial: explicitly not a completed failover
    }
    return 0;
  }

  return usage();
}
