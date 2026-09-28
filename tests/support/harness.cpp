#include "harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#  include <process.h>
#else
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace cf_test {
namespace {

namespace cf = cooling_failover;

std::uint64_t process_id() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::wstring widen(const std::string& text) {
  std::wstring wide;
  wide.reserve(text.size());
  for (char c : text) {
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return wide;
}

std::uint64_t temp_counter() {
  static std::uint64_t counter = 0;
  return ++counter;
}

}  // namespace

TempDir::TempDir(std::string_view tag) {
  std::error_code code;
  const std::filesystem::path base =
      std::filesystem::temp_directory_path(code) / "cooling-failover-tests";
  std::filesystem::create_directories(base, code);
  std::ostringstream name;
  name << tag << "-" << process_id() << "-" << temp_counter();
  path_ = base / name.str();
  std::filesystem::create_directories(path_, code);
}

TempDir::~TempDir() {
  std::error_code code;
  std::filesystem::remove_all(path_, code);
  // Remove the shared parent when it is empty. Best effort only.
  std::filesystem::path parent = path_.parent_path();
  if (!parent.empty()) {
    std::filesystem::remove(parent, code);
  }
}

std::filesystem::path TempDir::root() const {
  std::error_code code;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(path_, code);
  return code ? path_ : canonical;
}

DomainHarness::DomainHarness(std::filesystem::path root_path, cf::synthetic::SyntheticSpec spec)
    : root(std::move(root_path)),
      facility(std::move(spec)),
      airflow(cf::OwnerSystem::AirflowControl, "synthetic-airflow"),
      liquid(cf::OwnerSystem::LiquidCoolingControl, "synthetic-liquid") {}

cf::FailoverDomainId DomainHarness::domain() const { return facility.domain(); }

cf::Result<std::unique_ptr<DomainHarness>> DomainHarness::open(
    const std::filesystem::path& root_path, cf::synthetic::SyntheticSpec spec) {
  const cf::FailoverDomainId domain_id = spec.domain;
  auto harness = std::make_unique<DomainHarness>(root_path, std::move(spec));
  cf::OrchestratorOptions options;
  options.store.root = root_path;
  CF_TRY_ASSIGN(orchestrator, cf::CoolingFailoverOrchestrator::open(options, domain_id));
  orchestrator->set_control_runtime(cf::OwnerSystem::AirflowControl, &harness->airflow);
  orchestrator->set_control_runtime(cf::OwnerSystem::LiquidCoolingControl, &harness->liquid);
  harness->orchestrator = std::move(orchestrator);
  harness->airflow.set_generations(harness->facility.spec().topology_generation,
                                   harness->facility.spec().capacity_generation);
  harness->liquid.set_generations(harness->facility.spec().topology_generation,
                                  harness->facility.spec().capacity_generation);
  return harness;
}

cf::Result<void> DomainHarness::restart() {
  if (orchestrator != nullptr) {
    CF_TRY(orchestrator->close());
  }
  orchestrator.reset();
  cf::OrchestratorOptions options;
  options.store.root = root;
  CF_TRY_ASSIGN(reopened, cf::CoolingFailoverOrchestrator::open(options, facility.domain()));
  reopened->set_control_runtime(cf::OwnerSystem::AirflowControl, &airflow);
  reopened->set_control_runtime(cf::OwnerSystem::LiquidCoolingControl, &liquid);
  orchestrator = std::move(reopened);
  return cf::Result<void>();
}

cf::Result<void> DomainHarness::publish_topology() {
  CF_TRY(orchestrator->import_topology(facility.topology(new_evidence(), now)));
  return cf::Result<void>();
}

cf::Result<void> DomainHarness::publish_capacity() {
  CF_TRY(orchestrator->record_capacity(facility.capacity(new_evidence(), now)));
  return cf::Result<void>();
}

cf::Result<void> DomainHarness::publish_policy() {
  CF_TRY(orchestrator->install_policy(facility.spec().policy));
  return cf::Result<void>();
}

cf::Result<void> DomainHarness::publish_obligations() {
  CF_TRY(orchestrator->install_obligations(facility.obligations(new_evidence(), now)));
  return cf::Result<void>();
}

cf::Result<void> DomainHarness::publish_authority(cf::AuthorityState state,
                                                  cf::ControlPlaneEpoch epoch) {
  facility.set_authority(state, epoch);
  CF_TRY(orchestrator->record_authority(facility.authority(new_evidence(), now)));
  return cf::Result<void>();
}

cf::Result<void> DomainHarness::publish_health(cf::CoolingSourceId source) {
  CF_TRY(orchestrator->record_health(facility.health_for(source, new_evidence(), now)));
  return cf::Result<void>();
}

cf::Result<void> DomainHarness::publish_all_health() {
  for (const cf::synthetic::SyntheticGroup& group : facility.spec().groups) {
    CF_TRY(publish_health(group.source));
  }
  return cf::Result<void>();
}

cf::Result<void> DomainHarness::bootstrap(cf::ControlPlaneEpoch epoch) {
  CF_TRY(orchestrator->register_domain(now));
  CF_TRY(publish_topology());
  CF_TRY(publish_capacity());
  CF_TRY(publish_policy());
  CF_TRY(publish_obligations());
  CF_TRY(publish_authority(cf::AuthorityState::Granted, epoch));
  CF_TRY(publish_all_health());
  CF_TRY(orchestrator->establish_epoch(epoch, now));
  return cf::Result<void>();
}

const std::string& child_executable() {
  static const std::string path = CF_TEST_CHILD_PATH;
  return path;
}

ChildResult run_child(const std::string& mode, const std::vector<std::string>& args) {
  ChildProcess process;
  ChildResult launched = ChildProcess::launch(mode, args, process);
  if (!launched.started) {
    return launched;
  }
  ChildResult result;
  result.started = true;
  result.exit_code = process.wait();
  return result;
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : process_(other.process_), waited_(other.waited_), exit_code_(other.exit_code_) {
  other.process_ = nullptr;
  other.waited_ = false;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (process_ != nullptr) {
      terminate();
    }
    process_ = other.process_;
    waited_ = other.waited_;
    exit_code_ = other.exit_code_;
    other.process_ = nullptr;
    other.waited_ = false;
  }
  return *this;
}

ChildProcess::~ChildProcess() {
  if (process_ != nullptr) {
    terminate();
  }
}

ChildResult ChildProcess::launch(const std::string& mode, const std::vector<std::string>& args,
                                 ChildProcess& out) {
  ChildResult result;
  std::string command = "\"" + child_executable() + "\" " + mode;
  for (const std::string& argument : args) {
    command += " \"";
    command += argument;
    command += "\"";
  }
#ifdef _WIN32
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  std::wstring mutable_command = widen(command);
  if (::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                       &startup, &info) == 0) {
    result.error = "CreateProcess failed with code " + std::to_string(::GetLastError());
    return result;
  }
  ::CloseHandle(info.hThread);
  out.process_ = info.hProcess;
#else
  const pid_t child = ::fork();
  if (child < 0) {
    result.error = "fork failed";
    return result;
  }
  if (child == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(child_executable().c_str()));
    argv.push_back(const_cast<char*>(mode.c_str()));
    for (const std::string& argument : args) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(child_executable().c_str(), argv.data());
    ::_exit(127);
  }
  out.process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(child));
#endif
  result.started = true;
  return result;
}

bool ChildProcess::wait_for_marker(const std::filesystem::path& marker, std::string* contents) {
  for (;;) {
    std::error_code code;
    if (std::filesystem::exists(marker, code) && !code) {
      if (contents != nullptr) {
        std::ifstream stream(marker);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        *contents = buffer.str();
      }
      return true;
    }
    if (!running()) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

bool ChildProcess::running() {
  if (process_ == nullptr || waited_) {
    return false;
  }
#ifdef _WIN32
  const DWORD state = ::WaitForSingleObject(static_cast<HANDLE>(process_), 0);
  return state == WAIT_TIMEOUT;
#else
  const pid_t child = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  int status = 0;
  const pid_t outcome = ::waitpid(child, &status, WNOHANG);
  if (outcome == 0) {
    return true;
  }
  waited_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return false;
#endif
}

int ChildProcess::wait() {
  if (process_ == nullptr || waited_) {
    return exit_code_;
  }
#ifdef _WIN32
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  ::CloseHandle(static_cast<HANDLE>(process_));
  process_ = nullptr;
  waited_ = true;
  exit_code_ = static_cast<int>(code);
#else
  const pid_t child = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  int status = 0;
  ::waitpid(child, &status, 0);
  process_ = nullptr;
  waited_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  return exit_code_;
}

void ChildProcess::terminate() {
  if (process_ == nullptr || waited_) {
    return;
  }
#ifdef _WIN32
  ::TerminateProcess(static_cast<HANDLE>(process_), 0xDEAD);
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  ::CloseHandle(static_cast<HANDLE>(process_));
#else
  const pid_t child = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  ::kill(child, SIGKILL);
  int status = 0;
  ::waitpid(child, &status, 0);
#endif
  process_ = nullptr;
  waited_ = true;
}

std::map<std::string, std::string> read_result_file(const std::filesystem::path& path) {
  std::map<std::string, std::string> values;
  std::ifstream stream(path);
  if (!stream) {
    return values;
  }
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t split = line.find('=');
    if (split == std::string::npos) {
      continue;
    }
    values[line.substr(0, split)] = line.substr(split + 1);
  }
  return values;
}

void write_result_file(const std::filesystem::path& path,
                       const std::map<std::string, std::string>& values) {
  std::ofstream stream(path, std::ios::trunc);
  for (const auto& entry : values) {
    stream << entry.first << "=" << entry.second << "\n";
  }
}

}  // namespace cf_test
