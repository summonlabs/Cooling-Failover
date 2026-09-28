// Shared test harness: temporary directories, synthetic domain wiring and
// out-of-process helpers.
#pragma once

#include "cooling_failover/orchestrator.hpp"
#include "cooling_failover/synthetic.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cf_test {

/// RAII temporary directory outside the repository, removed on destruction.
class TempDir {
 public:
  explicit TempDir(std::string_view tag);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  /// Absolute canonical path used as the store root.
  [[nodiscard]] std::filesystem::path root() const;

 private:
  std::filesystem::path path_;
};

/// A synthetic domain wired to a real durable store and real OS file locking.
struct DomainHarness {
  std::filesystem::path root;
  cooling_failover::synthetic::SyntheticFacility facility;
  cooling_failover::synthetic::SyntheticControlRuntime airflow;
  cooling_failover::synthetic::SyntheticControlRuntime liquid;
  std::unique_ptr<cooling_failover::CoolingFailoverOrchestrator> orchestrator;
  cooling_failover::Tick now{};
  std::uint64_t next_evidence{1000};

  DomainHarness(std::filesystem::path root_path, cooling_failover::synthetic::SyntheticSpec spec);

  [[nodiscard]] cooling_failover::EvidenceId new_evidence() {
    next_evidence += 1;
    return cooling_failover::EvidenceId::from_value(next_evidence);
  }

  [[nodiscard]] cooling_failover::FailoverDomainId domain() const;

  /// Opens the orchestrator over \p root (creating it when needed) and attaches
  /// the synthetic control runtimes.
  static cooling_failover::Result<std::unique_ptr<DomainHarness>> open(
      const std::filesystem::path& root, cooling_failover::synthetic::SyntheticSpec spec);

  /// Re-opens an existing store, simulating a process restart.
  [[nodiscard]] cooling_failover::Result<void> restart();

  [[nodiscard]] cooling_failover::Result<void> publish_topology();
  [[nodiscard]] cooling_failover::Result<void> publish_capacity();
  [[nodiscard]] cooling_failover::Result<void> publish_policy();
  [[nodiscard]] cooling_failover::Result<void> publish_obligations();
  [[nodiscard]] cooling_failover::Result<void> publish_authority(
      cooling_failover::AuthorityState state, cooling_failover::ControlPlaneEpoch epoch);
  [[nodiscard]] cooling_failover::Result<void> publish_health(
      cooling_failover::CoolingSourceId source);
  [[nodiscard]] cooling_failover::Result<void> publish_all_health();

  /// Full bootstrap: register, epoch, topology, capacity, policy, obligations,
  /// authority and health for every source.
  [[nodiscard]] cooling_failover::Result<void> bootstrap(
      cooling_failover::ControlPlaneEpoch epoch);
};

/// Executes a child process and waits for it to terminate.
struct ChildResult {
  bool started{false};
  int exit_code{-1};
  std::string error{};
};

/// Path of the helper child executable built alongside the tests.
[[nodiscard]] const std::string& child_executable();

[[nodiscard]] ChildResult run_child(const std::string& mode, const std::vector<std::string>& args);

/// A child process that is expected to stay alive while the parent works.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess();

  [[nodiscard]] static ChildResult launch(const std::string& mode,
                                          const std::vector<std::string>& args,
                                          ChildProcess& out);

  /// Waits until \p marker exists, or until the child exits. Returns false when
  /// the child terminated without producing the marker.
  [[nodiscard]] bool wait_for_marker(const std::filesystem::path& marker,
                                     std::string* contents);
  /// Waits for the child to terminate and returns its exit code.
  [[nodiscard]] int wait();
  [[nodiscard]] bool running();
  void terminate();
  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

 private:
  void* process_{nullptr};
  bool waited_{false};
  int exit_code_{-1};
};

/// Reads a "key=value" result file written by a child process.
[[nodiscard]] std::map<std::string, std::string> read_result_file(
    const std::filesystem::path& path);

/// Writes a "key=value" result file.
void write_result_file(const std::filesystem::path& path,
                       const std::map<std::string, std::string>& values);

}  // namespace cf_test
