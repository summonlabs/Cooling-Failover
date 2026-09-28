// Durable store: integrity, truncation, corruption, checkpoint and crash windows.
#include "harness.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace cooling_failover;

namespace {

const ControlPlaneEpoch kEpoch = ControlPlaneEpoch::from_value(1);

void copy_directory(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code code;
  std::filesystem::remove_all(to, code);
  std::filesystem::create_directories(to, code);
  std::filesystem::copy(from, to,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing,
                        code);
  CF_CHECK_MSG(!code, "directory copy failed: " + code.message());
}

std::filesystem::path wal_path(const std::filesystem::path& root, FailoverDomainId domain) {
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(root);
  std::string name;
  static constexpr char kDigits[] = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4) {
    name.push_back(kDigits[(domain.value() >> shift) & 0xFU]);
  }
  return canonical / name / "wal.log";
}

void restore_directory(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code code;
  std::filesystem::remove_all(to, code);
  std::filesystem::create_directories(to.parent_path(), code);
  std::filesystem::copy(from, to,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing,
                        code);
  CF_CHECK_MSG(!code, "directory restore failed: " + code.message());
}

std::uintmax_t size_of(const std::filesystem::path& path) {
  std::error_code code;
  const std::uintmax_t size = std::filesystem::file_size(path, code);
  CF_CHECK_MSG(!code, "file size unavailable");
  return size;
}

void truncate_to(const std::filesystem::path& path, std::uintmax_t size) {
  std::error_code code;
  std::filesystem::resize_file(path, size, code);
  CF_CHECK_MSG(!code, "resize failed");
}

std::unique_ptr<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
  auto bytes = std::make_unique<std::vector<std::uint8_t>>();
  std::ifstream stream(path, std::ios::binary);
  CF_CHECK_MSG(static_cast<bool>(stream), "file could not be opened");
  char value = 0;
  while (stream.read(&value, 1)) {
    bytes->push_back(static_cast<std::uint8_t>(value));
  }
  return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  CF_CHECK_MSG(static_cast<bool>(stream), "file could not be written");
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

void flip_byte(const std::filesystem::path& path, std::uintmax_t offset) {
  std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
  CF_CHECK(static_cast<bool>(stream));
  stream.seekg(static_cast<std::streamoff>(offset));
  char value = 0;
  stream.read(&value, 1);
  value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x5AU);
  stream.seekp(static_cast<std::streamoff>(offset));
  stream.write(&value, 1);
}

/// Builds a domain with \p extra health observations on top of the bootstrap.
cooling_failover::Result<std::uint64_t> build_store(const std::filesystem::path& root,
                                                    FailoverDomainId domain,
                                                    std::uint64_t extra) {
  CF_TRY_ASSIGN(harness, cf_test::DomainHarness::open(root, synthetic::two_group_plant(domain)));
  CF_TRY(harness->bootstrap(kEpoch));
  for (std::uint64_t index = 0; index < extra; ++index) {
    harness->now = Tick::from_value(index + 1);
    CF_TRY(harness->publish_health(CoolingSourceId::from_value(1)));
  }
  const std::uint64_t commit = harness->orchestrator->state().last_commit.value();
  CF_TRY(harness->orchestrator->close());
  return commit;
}

}  // namespace

CF_TEST(Persistence, RoundTripSurvivesRestart) {
  cf_test::TempDir dir("persist-roundtrip");
  const FailoverDomainId domain = FailoverDomainId::from_value(40);
  std::uint64_t commit = 0;
  CF_ASSIGN_OR_FAIL(built, build_store(dir.root(), domain, 2));
  commit = built;
  CF_CHECK(commit > 0);

  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(dir.root(),
                                                          synthetic::two_group_plant(domain)));
  CF_CHECK_EQ(harness->orchestrator->state().last_commit.value(), commit);
  CF_CHECK(harness->orchestrator->state().registered);
  CF_CHECK(harness->orchestrator->state().epoch_established);
  CF_CHECK(harness->orchestrator->state().has_topology);
  CF_CHECK(harness->orchestrator->state().has_capacity);
  CF_CHECK(harness->orchestrator->state().has_policy);
  CF_CHECK(harness->orchestrator->state().has_obligations);
  CF_CHECK(harness->orchestrator->state().has_authority);
  CF_CHECK(harness->orchestrator->recovery().records_replayed > 0);
  const std::string first = harness->orchestrator->state_digest().to_hex();

  // A second reopen from the same durable bytes yields the same canonical state.
  CF_CHECK_OK(harness->orchestrator->close());
  CF_ASSIGN_OR_FAIL(second_harness, cf_test::DomainHarness::open(
                                        dir.root(), synthetic::two_group_plant(domain)));
  CF_CHECK_EQ(second_harness->orchestrator->state_digest().to_hex(), first);
}

CF_TEST(Persistence, CheckpointPreservesStateAndCommit) {
  cf_test::TempDir dir("persist-checkpoint");
  const FailoverDomainId domain = FailoverDomainId::from_value(41);
  std::uint64_t commit = 0;
  CF_ASSIGN_OR_FAIL(built, build_store(dir.root(), domain, 3));
  commit = built;

  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(dir.root(),
                                                          synthetic::two_group_plant(domain)));
  const std::string before = harness->orchestrator->state_digest().to_hex();
  CF_CHECK_OK(harness->orchestrator->checkpoint());
  CF_CHECK_EQ(harness->orchestrator->state().last_commit.value(), commit);
  CF_CHECK_EQ(harness->orchestrator->state_digest().to_hex(), before);
  // The store stays usable and keeps counting from the same commit sequence.
  harness->now = Tick::from_value(99);
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
  CF_CHECK_EQ(harness->orchestrator->state().last_commit.value(), commit + 1);
  CF_CHECK_OK(harness->orchestrator->close());

  CF_ASSIGN_OR_FAIL(reopened, cf_test::DomainHarness::open(dir.root(),
                                                           synthetic::two_group_plant(domain)));
  CF_CHECK_EQ(reopened->orchestrator->state().last_commit.value(), commit + 1);
  CF_CHECK(reopened->orchestrator->recovery().snapshot_loaded);
}

CF_TEST(Persistence, CheckpointCrashWindowResolvesToTheSnapshotGeneration) {
  cf_test::TempDir dir("persist-window");
  const FailoverDomainId domain = FailoverDomainId::from_value(42);
  CF_ASSIGN_OR_FAIL(built, build_store(dir.root(), domain, 4));
  const std::uint64_t commit = built;

  cf_test::TempDir backup_dir("persist-window-backup");
  const std::filesystem::path backup = backup_dir.path();
  copy_directory(dir.root(), backup);
  const std::filesystem::path saved_wal = backup / "wal-copy.log";
  std::error_code code;
  std::filesystem::copy_file(wal_path(backup, domain), saved_wal,
                             std::filesystem::copy_options::overwrite_existing, code);
  CF_CHECK_MSG(!code, "wal backup failed");

  {
    CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(dir.root(),
                                                            synthetic::two_group_plant(domain)));
    CF_CHECK_OK(harness->orchestrator->checkpoint());
    CF_CHECK_OK(harness->orchestrator->close());
  }

  // Crash window: the snapshot was published but the write-ahead log still
  // holds the superseded records.
  std::filesystem::copy_file(saved_wal, wal_path(dir.root(), domain),
                             std::filesystem::copy_options::overwrite_existing, code);
  CF_CHECK_MSG(!code, "wal restore failed");

  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(dir.root(),
                                                          synthetic::two_group_plant(domain)));
  CF_CHECK_EQ(harness->orchestrator->state().last_commit.value(), commit);
  CF_CHECK(harness->orchestrator->recovery().snapshot_loaded);
}

CF_TEST(Persistence, TornTailResolvesToThePreviousCompleteGeneration) {
  cf_test::TempDir dir("persist-torn");
  const FailoverDomainId domain = FailoverDomainId::from_value(43);
  CF_ASSIGN_OR_FAIL(clean, build_store(dir.root(), domain, 2));
  const std::uint64_t clean_commit = clean;

  // A second process appends a batch and dies without publishing the rollback
  // guard. This is exactly the durable situation after a crash mid-append.
  const std::filesystem::path append_result = dir.path() / "append.txt";
  const cf_test::ChildResult appended = cf_test::run_child(
      "append", {dir.root().string(), std::to_string(domain.value()), append_result.string(), "4"});
  CF_CHECK_EQ(appended.exit_code, 17);
  const auto values = cf_test::read_result_file(append_result);
  const std::uint64_t crashed_commit = std::stoull(values.at("commit_after"));
  CF_CHECK(crashed_commit > clean_commit);
  CF_CHECK_EQ(std::stoull(values.at("commit_before")), clean_commit);

  const std::filesystem::path wal = wal_path(dir.root(), domain);
  const std::uintmax_t size = size_of(wal);

  cf_test::TempDir pristine_dir("persist-torn-pristine");
  copy_directory(dir.root(), pristine_dir.path());
  const std::filesystem::path live_domain = wal.parent_path();
  const std::filesystem::path pristine_domain = wal_path(pristine_dir.path(), domain).parent_path();

  const std::uintmax_t deltas[] = {1, 17, 40};
  for (std::uintmax_t delta : deltas) {
    restore_directory(pristine_domain, live_domain);
    truncate_to(wal, size - delta);
    CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                   dir.root(), synthetic::two_group_plant(domain)));
    const std::uint64_t resolved = harness->orchestrator->state().last_commit.value();
    CF_CHECK_MSG(resolved < crashed_commit, "a torn tail must not resolve to the truncated record");
    CF_CHECK_MSG(resolved >= clean_commit, "recovery must never fall below a durable generation");
    CF_CHECK(harness->orchestrator->recovery().discarded_torn_tail);
    CF_CHECK(harness->orchestrator->recovery().discarded_tail_bytes > 0);
    // The store stays usable: the torn tail was removed and the log continues.
    harness->now = Tick::from_value(1000);
    CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
    CF_CHECK_EQ(harness->orchestrator->state().last_commit.value(), resolved + 1);
    CF_CHECK_OK(harness->orchestrator->close());
  }
}

CF_TEST(Persistence, PayloadCorruptionIsDetectedAndNeverSilentlyAccepted) {
  cf_test::TempDir dir("persist-corrupt");
  const FailoverDomainId domain = FailoverDomainId::from_value(44);
  CF_ASSIGN_OR_FAIL(clean, build_store(dir.root(), domain, 2));
  const std::uint64_t clean_commit = clean;

  const std::filesystem::path append_result = dir.path() / "append.txt";
  const cf_test::ChildResult appended = cf_test::run_child(
      "append", {dir.root().string(), std::to_string(domain.value()), append_result.string(), "4"});
  CF_CHECK_EQ(appended.exit_code, 17);
  const std::uint64_t crashed_commit =
      std::stoull(cf_test::read_result_file(append_result).at("commit_after"));

  const std::filesystem::path wal = wal_path(dir.root(), domain);
  const std::uintmax_t size = size_of(wal);

  cf_test::TempDir pristine_dir("persist-corrupt-pristine");
  copy_directory(dir.root(), pristine_dir.path());
  const std::filesystem::path live_domain = wal.parent_path();
  const std::filesystem::path pristine_domain = wal_path(pristine_dir.path(), domain).parent_path();
  const std::filesystem::path pristine_wal = wal_path(pristine_dir.path(), domain);

  // A single flipped byte inside the last record must never be accepted.
  std::uintmax_t offset = size - 8;
  for (int attempt = 0; attempt < 3; ++attempt) {
    restore_directory(pristine_domain, live_domain);
    flip_byte(wal, offset);
    CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                   dir.root(), synthetic::two_group_plant(domain)));
    const std::uint64_t resolved = harness->orchestrator->state().last_commit.value();
    CF_CHECK(resolved < crashed_commit);
    CF_CHECK(resolved >= clean_commit);
    CF_CHECK_OK(harness->orchestrator->close());
    offset = offset > 4 ? offset - 4 : 0;
  }

  // Header corruption is a hard failure, never a silent reset.
  restore_directory(pristine_domain, live_domain);
  flip_byte(wal, 3);
  CF_CHECK_ERROR(cf_test::DomainHarness::open(dir.root(), synthetic::two_group_plant(domain)),
                 ErrorCode::StoreCorrupt);

  restore_directory(pristine_domain, live_domain);
  flip_byte(wal, 20);  // inside the first record header
  {
    cooling_failover::Result<std::unique_ptr<cf_test::DomainHarness>> outcome =
        cf_test::DomainHarness::open(dir.root(), synthetic::two_group_plant(domain));
    CF_CHECK(!outcome.has_value());
    CF_CHECK(outcome.status().code() == ErrorCode::StoreCorrupt ||
             outcome.status().code() == ErrorCode::StoreTruncated);
  }

  // A clean restore opens exactly at the crash generation.
  restore_directory(pristine_domain, live_domain);
  CF_ASSIGN_OR_FAIL(restored, cf_test::DomainHarness::open(
                                  dir.root(), synthetic::two_group_plant(domain)));
  CF_CHECK_EQ(restored->orchestrator->state().last_commit.value(), crashed_commit);
  CF_CHECK_EQ(restored->orchestrator->recovery().discarded_tail_bytes, std::uint64_t{0});
  CF_CHECK_OK(restored->orchestrator->close());
  (void)pristine_wal;
}

CF_TEST(Persistence, RollbackBelowTheGuardIsRefused) {
  cf_test::TempDir dir("persist-rollback");
  const FailoverDomainId domain = FailoverDomainId::from_value(45);
  CF_ASSIGN_OR_FAIL(built, build_store(dir.root(), domain, 6));
  const std::uint64_t commit = built;
  (void)commit;
  const std::filesystem::path wal = wal_path(dir.root(), domain);
  truncate_to(wal, 32);  // keep only the header
  CF_CHECK_ERROR(cf_test::DomainHarness::open(dir.root(), synthetic::two_group_plant(domain)),
                 ErrorCode::StoreRolledBack);
}

CF_TEST(Persistence, MetadataTamperingIsRefused) {
  cf_test::TempDir dir("persist-meta");
  const FailoverDomainId domain = FailoverDomainId::from_value(46);
  CF_ASSIGN_OR_FAIL(built, build_store(dir.root(), domain, 1));
  (void)built;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(dir.root());
  std::string name;
  static constexpr char kDigits[] = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4) {
    name.push_back(kDigits[(domain.value() >> shift) & 0xFU]);
  }
  const std::filesystem::path meta = canonical / name / "store.meta";
  CF_CHECK(std::filesystem::exists(meta));

  // Any tampering that leaves the integrity check broken is corruption.
  flip_byte(meta, 16);  // domain identity lives here
  CF_CHECK_ERROR(cf_test::DomainHarness::open(dir.root(), synthetic::two_group_plant(domain)),
                 ErrorCode::StoreCorrupt);
  flip_byte(meta, 16);

  // A metadata record that is internally consistent but declares an unknown
  // format version is refused as unsupported rather than misread.
  const auto rewrite_meta = [&](std::uint32_t version, std::uint32_t reserved) {
    std::vector<std::uint8_t> bytes = *read_file(meta);
    CF_CHECK(bytes.size() == 64);
    bytes[8] = static_cast<std::uint8_t>(version & 0xFFU);
    bytes[9] = static_cast<std::uint8_t>((version >> 8) & 0xFFU);
    bytes[10] = static_cast<std::uint8_t>((version >> 16) & 0xFFU);
    bytes[11] = static_cast<std::uint8_t>((version >> 24) & 0xFFU);
    bytes[12] = static_cast<std::uint8_t>(reserved & 0xFFU);
    const std::uint32_t crc = crc32(std::span<const std::uint8_t>(bytes.data(), 60));
    bytes[60] = static_cast<std::uint8_t>(crc & 0xFFU);
    bytes[61] = static_cast<std::uint8_t>((crc >> 8) & 0xFFU);
    bytes[62] = static_cast<std::uint8_t>((crc >> 16) & 0xFFU);
    bytes[63] = static_cast<std::uint8_t>((crc >> 24) & 0xFFU);
    write_file(meta, bytes);
  };

  rewrite_meta(999, 0);
  CF_CHECK_ERROR(cf_test::DomainHarness::open(dir.root(), synthetic::two_group_plant(domain)),
                 ErrorCode::StoreVersionUnsupported);

  rewrite_meta(1, 7);
  CF_CHECK_ERROR(cf_test::DomainHarness::open(dir.root(), synthetic::two_group_plant(domain)),
                 ErrorCode::StoreReservedFieldSet);

  rewrite_meta(1, 0);
  CF_ASSIGN_OR_FAIL(restored, cf_test::DomainHarness::open(
                                  dir.root(), synthetic::two_group_plant(domain)));
  CF_CHECK_OK(restored->orchestrator->close());
}

CF_TEST(Persistence, RecoveredObservationsAreNotCurrentEvidence) {
  cf_test::TempDir dir("persist-recovered");
  const FailoverDomainId domain = FailoverDomainId::from_value(47);
  {
    CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                   dir.root(), synthetic::two_group_plant(domain)));
    CF_CHECK_OK(harness->bootstrap(kEpoch));
    harness->now = Tick::from_value(5);
    harness->facility.fail_all_except({SourceGroupId::from_value(2)});
    CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
    CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(2)));
    PlanRequest request;
    request.request = RequestId::from_value(1);
    request.now = harness->now;
    request.kind = PlanKind::Failover;
    request.incumbent = SourceGroupId::from_value(1);
    request.reason = "primary unavailable";
    CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(request));
    CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
    CF_CHECK_EQ(attempt.state, AttemptState::Verified);
    CF_CHECK(!harness->orchestrator->state().observations.empty());
    CF_CHECK_OK(harness->orchestrator->close());
  }

  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(domain)));
  CF_CHECK(!harness->orchestrator->state().observations.empty());
  for (const auto& entry : harness->orchestrator->state().observations) {
    CF_CHECK(entry.second.recovered_unvalidated);
  }
  CF_CHECK(harness->orchestrator->state().streaks.empty());
}
