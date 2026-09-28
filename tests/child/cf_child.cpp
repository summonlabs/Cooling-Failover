// Cooling Failover - helper child process for out-of-process proof.
//
// Every mode is deterministic and non-interactive: it never prompts and always
// terminates with an explicit exit code. Modes that simulate a crash call
// std::_Exit so that no destructor, buffer flush or cleanup path runs.
#include "cooling_failover/orchestrator.hpp"
#include "cooling_failover/synthetic.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace cf = cooling_failover;
using namespace cooling_failover::synthetic;

namespace {

constexpr int kCrashExitCode = 17;

std::string to_text(std::uint64_t value) { return std::to_string(value); }

void write_result(const std::string& path, const std::map<std::string, std::string>& values) {
  if (path.empty()) {
    return;
  }
  std::ofstream stream(path, std::ios::trunc);
  for (const auto& entry : values) {
    stream << entry.first << "=" << entry.second << "\n";
  }
}

cf::FailoverDomainId parse_domain(const std::string& text) {
  return cf::FailoverDomainId::from_value(std::stoull(text));
}

struct Setup {
  explicit Setup(cf::FailoverDomainId domain)
      : facility(two_group_plant(domain)),
        airflow(std::make_unique<SyntheticControlRuntime>(cf::OwnerSystem::AirflowControl,
                                                          "child-airflow")),
        liquid(std::make_unique<SyntheticControlRuntime>(cf::OwnerSystem::LiquidCoolingControl,
                                                         "child-liquid")) {}

  SyntheticFacility facility;
  std::unique_ptr<SyntheticControlRuntime> airflow;
  std::unique_ptr<SyntheticControlRuntime> liquid;
  std::unique_ptr<cf::CoolingFailoverOrchestrator> orchestrator;
  cf::Tick now{};

  [[nodiscard]] cf::Result<void> open(const std::filesystem::path& root) {
    cf::OrchestratorOptions options;
    options.store.root = root;
    CF_TRY_ASSIGN(opened, cf::CoolingFailoverOrchestrator::open(options, facility.domain()));
    opened->set_control_runtime(cf::OwnerSystem::AirflowControl, airflow.get());
    opened->set_control_runtime(cf::OwnerSystem::LiquidCoolingControl, liquid.get());
    orchestrator = std::move(opened);
    return cf::Result<void>();
  }

  [[nodiscard]] static cf::EvidenceId evidence(std::uint64_t value) {
    return cf::EvidenceId::from_value(value);
  }

  [[nodiscard]] cf::Result<void> bootstrap(cf::ControlPlaneEpoch epoch) {
    CF_TRY(orchestrator->register_domain(now));
    CF_TRY(orchestrator->import_topology(facility.topology(evidence(1), now)));
    CF_TRY(orchestrator->record_capacity(facility.capacity(evidence(2), now)));
    CF_TRY(orchestrator->install_policy(facility.spec().policy));
    CF_TRY(orchestrator->install_obligations(facility.obligations(evidence(3), now)));
    CF_TRY(orchestrator->record_authority(facility.authority(evidence(4), now)));
    for (const SyntheticGroup& group : facility.spec().groups) {
      CF_TRY(orchestrator->record_health(
          facility.health_for(group.source, evidence(100 + group.group.value()), now)));
    }
    CF_TRY(orchestrator->establish_epoch(epoch, now));
    return cf::Result<void>();
  }

  /// Publishes one health observation per synthetic source at \p tick.
  [[nodiscard]] cf::Result<void> observe_health(std::uint64_t tick, cf::HealthState state) {
    now = cf::Tick::from_value(tick);
    for (const SyntheticGroup& group : facility.spec().groups) {
      facility.set_health(group.source, state);
      CF_TRY(orchestrator->record_health(facility.health_for(
          group.source, evidence(10000 + tick * 16 + group.group.value()), now)));
    }
    return cf::Result<void>();
  }
};

[[nodiscard]] cf::Result<void> run_mode(const std::string& mode, int argc, char** argv) {
  const std::string root = argc > 2 ? argv[2] : std::string();
  const cf::FailoverDomainId domain = argc > 3 ? parse_domain(argv[3]) : cf::FailoverDomainId::nil();
  const std::string result_path = argc > 4 ? argv[4] : std::string();

  if (mode == "hold") {
    // hold <root> <domain> <marker> <release>
    Setup setup(domain);
    const std::string marker = argv[4];
    const std::string release = argv[5];
    const cf::Result<void> opened = setup.open(root);
    if (!opened.has_value()) {
      write_result(marker, {{"state", "error"},
                            {"code", cf::to_string(opened.status().code())},
                            {"code_value", to_text(static_cast<std::uint64_t>(
                                               opened.status().code()))}});
      return cf::Result<void>();
    }
    write_result(marker, {{"state", "opened"},
                          {"pid", to_text(static_cast<std::uint64_t>(
#ifdef _WIN32
                                     ::_getpid()
#else
                                     ::getpid()
#endif
                                     ))}});
    for (;;) {
      std::error_code code;
      if (std::filesystem::exists(release, code) && !code) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cf::Result<void>();
  }

  if (mode == "fill" || mode == "append") {
    // fill|append <root> <domain> <result> <count>
    Setup setup(domain);
    CF_TRY(setup.open(root));
    const std::uint64_t count = argc > 5 ? std::stoull(argv[5]) : 0;
    const std::uint64_t before = setup.orchestrator->state().last_commit.value();
    if (mode == "fill") {
      CF_TRY(setup.bootstrap(cf::ControlPlaneEpoch::from_value(1)));
    }
    // Continue the logical clock from whatever the store already holds so that
    // an appended batch is never dated before existing evidence.
    const std::uint64_t base_tick = setup.orchestrator->state().last_tick.value();
    setup.now = cf::Tick::from_value(base_tick);
    for (std::uint64_t index = 0; index < count; ++index) {
      CF_TRY(setup.observe_health(base_tick + index + 1, cf::HealthState::Healthy));
    }
    write_result(result_path,
                 {{"ok", "1"},
                  {"commit_before", to_text(before)},
                  {"commit_after", to_text(setup.orchestrator->state().last_commit.value())},
                  {"sources", to_text(setup.facility.spec().groups.size())}});
    if (mode == "append") {
      std::_Exit(kCrashExitCode);  // abrupt death: no flush, no cleanup, no unwinding
    }
    return cf::Result<void>();
  }

  if (mode == "scenario-begin") {
    // scenario-begin <root> <domain> <result>
    Setup setup(domain);
    CF_TRY(setup.open(root));
    CF_TRY(setup.bootstrap(cf::ControlPlaneEpoch::from_value(1)));
    SyntheticRuntimeBehaviour quiet;
    quiet.emits_effects = false;  // the runtime acknowledges but observes nothing
    setup.airflow->set_behaviour(quiet);
    setup.liquid->set_behaviour(quiet);

    cf::PlanRequest request;
    request.request = cf::RequestId::from_value(9001);
    request.now = setup.now;
    request.kind = cf::PlanKind::Failover;
    request.incumbent = cf::SourceGroupId::from_value(1);
    request.reason = "primary chiller unavailable";
    CF_TRY_ASSIGN(plan, setup.orchestrator->create_plan(request));
    CF_TRY_ASSIGN(attempt, setup.orchestrator->execute_plan(plan.id, setup.now));
    write_result(result_path,
                 {{"ok", "1"},
                  {"plan", to_text(plan.id.value())},
                  {"attempt", to_text(attempt.id.value())},
                  {"state", cf::to_string(attempt.state)},
                  {"actuations",
                   to_text(setup.airflow->actuations() + setup.liquid->actuations())},
                  {"commands", to_text(setup.orchestrator->state().commands.size())}});
    std::_Exit(kCrashExitCode);
  }

  if (mode == "scenario-continue") {
    // scenario-continue <root> <domain> <result>
    Setup setup(domain);
    CF_TRY(setup.open(root));
    CF_TRY_ASSIGN(status, setup.orchestrator->status());
    if (status.active_plan.is_nil()) {
      write_result(result_path, {{"ok", "0"}, {"reason", "no active plan"}});
      return cf::Result<void>();
    }
    CF_TRY_ASSIGN(attempt, setup.orchestrator->execute_plan(status.active_plan, setup.now));
    write_result(result_path,
                 {{"ok", "1"},
                  {"plan", to_text(status.active_plan.value())},
                  {"attempt", to_text(attempt.id.value())},
                  {"state", cf::to_string(attempt.state)},
                  {"actuations",
                   to_text(setup.airflow->actuations() + setup.liquid->actuations())},
                  {"issue_calls",
                   to_text(setup.airflow->issue_calls() + setup.liquid->issue_calls())},
                  {"commands", to_text(setup.orchestrator->state().commands.size())}});
    return cf::Result<void>();
  }

  if (mode == "verify-store") {
    // verify-store <root> <domain> <result>
    Setup setup(domain);
    CF_TRY(setup.open(root));
    write_result(result_path,
                 {{"ok", "1"},
                  {"commit", to_text(setup.orchestrator->state().last_commit.value())},
                  {"revision", to_text(setup.orchestrator->state().revision.value())},
                  {"observations", to_text(setup.orchestrator->state().observations.size())},
                  {"digest", setup.orchestrator->state_digest().to_hex()}});
    return cf::Result<void>();
  }

  return cf::Status::error(cf::ErrorCode::InvalidArgument, "unknown child mode", "mode", mode);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // Intentional crash modes must never raise interactive Windows crash UI.
  ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
  if (argc < 2) {
    std::fprintf(stderr, "usage: cf_child <mode> [args...]\n");
    return 2;
  }
  const std::string mode = argv[1];
  const std::string result_path = argc > 4 ? argv[4] : std::string();
  if (mode == "hold") {
    const int hold_argc = argc;
    char** hold_argv = argv;
    const cf::Result<void> outcome = run_mode(mode, hold_argc, hold_argv);
    if (!outcome.has_value()) {
      std::fprintf(stderr, "hold: %s\n", outcome.status().canonical_line().c_str());
      return 1;
    }
    return 0;
  }
  const cf::Result<void> outcome = run_mode(mode, argc, argv);
  if (!outcome.has_value()) {
    write_result(result_path, {{"ok", "0"}, {"status", outcome.status().canonical_line()}});
    std::fprintf(stderr, "%s: %s\n", mode.c_str(), outcome.status().canonical_line().c_str());
    return 1;
  }
  return 0;
}
