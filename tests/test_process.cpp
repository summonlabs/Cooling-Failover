// Out-of-process proof: writer exclusion, abrupt death, restart without replay.
#include "harness.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace cooling_failover;

namespace {

const ControlPlaneEpoch kEpoch = ControlPlaneEpoch::from_value(1);

}  // namespace

CF_TEST(Process, WriterAuthorityIsExclusiveAcrossRealProcesses) {
  cf_test::TempDir dir("process-lock");
  const FailoverDomainId domain = FailoverDomainId::from_value(50);
  const std::filesystem::path marker = dir.path() / "marker.txt";
  const std::filesystem::path release = dir.path() / "release.txt";

  cf_test::ChildProcess child;
  const cf_test::ChildResult launched = cf_test::ChildProcess::launch(
      "hold", {dir.root().string(), std::to_string(domain.value()), marker.string(),
               release.string()},
      child);
  CF_CHECK_MSG(launched.started, launched.error);

  std::string contents;
  CF_CHECK_MSG(child.wait_for_marker(marker, &contents), "child never reported readiness");
  CF_CHECK_MSG(contents.find("state=opened") != std::string::npos, contents);

  // While the child holds writer authority no other process can obtain it.
  OrchestratorOptions options;
  options.store.root = dir.root();
  CF_CHECK_ERROR(CoolingFailoverOrchestrator::open(options, domain), ErrorCode::StoreLocked);

  { std::ofstream stream(release); stream << "go\n"; }
  CF_CHECK_EQ(child.wait(), 0);

  // Once the holder exits, authority is available again.
  CF_ASSIGN_OR_FAIL(reopened, CoolingFailoverOrchestrator::open(options, domain));
  CF_CHECK_OK(reopened->close());
}

CF_TEST(Process, WriterDeathReleasesAuthority) {
  cf_test::TempDir dir("process-death");
  const FailoverDomainId domain = FailoverDomainId::from_value(51);
  const std::filesystem::path marker = dir.path() / "marker.txt";
  const std::filesystem::path release = dir.path() / "never.txt";

  cf_test::ChildProcess child;
  const cf_test::ChildResult launched = cf_test::ChildProcess::launch(
      "hold", {dir.root().string(), std::to_string(domain.value()), marker.string(),
               release.string()},
      child);
  CF_CHECK_MSG(launched.started, launched.error);
  std::string contents;
  CF_CHECK_MSG(child.wait_for_marker(marker, &contents), "child never reported readiness");

  OrchestratorOptions options;
  options.store.root = dir.root();
  CF_CHECK_ERROR(CoolingFailoverOrchestrator::open(options, domain), ErrorCode::StoreLocked);

  child.terminate();  // abrupt death of the lock holder
  CF_ASSIGN_OR_FAIL(reopened, CoolingFailoverOrchestrator::open(options, domain));
  CF_CHECK_OK(reopened->close());
}

CF_TEST(Process, AbruptDeathAfterCommitsResolvesToACompleteGeneration) {
  cf_test::TempDir dir("process-crash");
  const FailoverDomainId domain = FailoverDomainId::from_value(52);
  const std::filesystem::path fill_result = dir.path() / "fill.txt";
  const std::filesystem::path append_result = dir.path() / "append.txt";

  cf_test::ChildResult filled = cf_test::run_child(
      "fill", {dir.root().string(), std::to_string(domain.value()), fill_result.string(), "2"});
  CF_CHECK_EQ(filled.exit_code, 0);
  const auto fill_values = cf_test::read_result_file(fill_result);
  CF_CHECK_EQ(fill_values.at("ok"), std::string("1"));

  cf_test::ChildResult appended = cf_test::run_child(
      "append", {dir.root().string(), std::to_string(domain.value()), append_result.string(), "4"});
  CF_CHECK_EQ(appended.exit_code, 17);  // abrupt death, not a graceful return
  const auto append_values = cf_test::read_result_file(append_result);
  const std::uint64_t before = std::stoull(append_values.at("commit_before"));
  const std::uint64_t after = std::stoull(append_values.at("commit_after"));
  const std::uint64_t sources = std::stoull(append_values.at("sources"));
  // One health record is committed per synthetic source per tick.
  CF_CHECK_EQ(after, before + 4 * sources);

  // A fresh process must observe exactly the committed generation.
  const std::filesystem::path verify_result = dir.path() / "verify.txt";
  cf_test::ChildResult verified = cf_test::run_child(
      "verify-store", {dir.root().string(), std::to_string(domain.value()), verify_result.string()});
  CF_CHECK_EQ(verified.exit_code, 0);
  const auto verify_values = cf_test::read_result_file(verify_result);
  CF_CHECK_EQ(std::stoull(verify_values.at("commit")), after);
  CF_CHECK_EQ(std::stoull(verify_values.at("revision")), after);
}

CF_TEST(Process, RestartDoesNotDuplicateAlreadyIssuedRequests) {
  cf_test::TempDir dir("process-replay");
  const FailoverDomainId domain = FailoverDomainId::from_value(53);
  const std::filesystem::path begin_result = dir.path() / "begin.txt";
  const std::filesystem::path continue_result = dir.path() / "continue.txt";

  cf_test::ChildResult began = cf_test::run_child(
      "scenario-begin", {dir.root().string(), std::to_string(domain.value()), begin_result.string()});
  CF_CHECK_EQ(began.exit_code, 17);  // the first process dies mid-transition
  const auto begin_values = cf_test::read_result_file(begin_result);
  CF_CHECK_EQ(begin_values.at("ok"), std::string("1"));
  const std::uint64_t actuations = std::stoull(begin_values.at("actuations"));
  const std::uint64_t commands = std::stoull(begin_values.at("commands"));
  CF_CHECK(actuations > 0);

  cf_test::ChildResult continued = cf_test::run_child(
      "scenario-continue",
      {dir.root().string(), std::to_string(domain.value()), continue_result.string()});
  CF_CHECK_EQ(continued.exit_code, 0);
  const auto continue_values = cf_test::read_result_file(continue_result);
  CF_CHECK_EQ(continue_values.at("ok"), std::string("1"));
  // Not one command reached the runtime again.
  CF_CHECK_EQ(std::stoull(continue_values.at("actuations")), std::uint64_t{0});
  CF_CHECK_EQ(std::stoull(continue_values.at("issue_calls")), std::uint64_t{0});
  CF_CHECK_EQ(std::stoull(continue_values.at("commands")), commands);
}

CF_TEST(Process, RecoveredAttemptIsMarkedInterruptedUntilReverified) {
  cf_test::TempDir dir("process-interrupted");
  const FailoverDomainId domain = FailoverDomainId::from_value(54);
  const std::filesystem::path begin_result = dir.path() / "begin.txt";
  cf_test::ChildResult began = cf_test::run_child(
      "scenario-begin", {dir.root().string(), std::to_string(domain.value()), begin_result.string()});
  CF_CHECK_EQ(began.exit_code, 17);

  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(domain)));
  CF_CHECK(!harness->orchestrator->state().active_plan.is_nil());
  const AttemptId attempt_id = harness->orchestrator->state().active_attempt;
  CF_CHECK(!attempt_id.is_nil());
  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->get_attempt(attempt_id));
  CF_CHECK_EQ(attempt.state, AttemptState::Interrupted);
  for (const auto& entry : harness->orchestrator->state().commands) {
    CF_CHECK(entry.second.outcome == CommandOutcome::Acknowledged);
  }
  // Re-verification completes the interrupted attempt with fresh evidence.
  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->get_plan(attempt.plan));
  CF_ASSIGN_OR_FAIL(resumed, harness->orchestrator->execute_plan(plan.id, harness->now));
  CF_CHECK_EQ(resumed.state, AttemptState::Verified);
  CF_CHECK_EQ(harness->liquid.actuations(), 0U);
}
