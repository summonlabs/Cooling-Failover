// Cooling Failover - durable single-writer store.
//
// Layout of one domain store directory:
//   writer.lock       OS-exclusive writer lock (canonical path, no reparse)
//   store.meta        magic + format version + domain identity (written once)
//   snapshot.dat      complete state at a commit sequence (atomic publication)
//   wal.log           append-only records after the snapshot
//   rollback.guard    highest commit sequence ever published (atomic publication)
//
// Commit point: a record is committed once its bytes have been flushed and the
// read-back verification matched. Recovery resolves to the last complete
// generation - never a hybrid - and truncates any torn append tail.
#pragma once

#include "cooling_failover/canonical.hpp"
#include "cooling_failover/export.hpp"
#include "cooling_failover/ids.hpp"
#include "cooling_failover/status.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace cooling_failover {

inline constexpr std::size_t kMaxStorePathChars = 4096;
inline constexpr std::size_t kMaxPathComponentChars = 255;

/// Record type tag stored in every WAL record. Values are part of the on-disk
/// format and never reused.
enum class RecordType : std::uint16_t {
  DomainRegistered = 10,
  EpochEstablished = 11,
  TopologyImported = 12,
  CapacityRecorded = 13,
  HealthRecorded = 14,
  AuthorityRecorded = 15,
  PolicyInstalled = 16,
  ObligationsInstalled = 17,
  PlanCreated = 20,
  PlanStateChanged = 21,
  CommandRecorded = 30,
  ObservationRecorded = 31,
  AttemptRecorded = 32,
  TransitionRecorded = 41,
};

/// True when p type is a record type understood by this format version.
[[nodiscard]] CF_API bool is_known_record_type(std::uint16_t raw) noexcept;

struct CF_API StoreOptions {
  /// Root directory that contains one subdirectory per failover domain.
  std::filesystem::path root{};
  /// Maximum number of records in one WAL before further commits are refused.
  std::uint64_t max_records{4'000'000};
  /// Maximum encoded payload size of one record.
  std::uint32_t max_record_bytes{1U << 20};
  /// Flush (and, on Windows, FlushFileBuffers) every commit.
  bool flush_on_commit{true};
  /// Read the record back after flushing and compare bytes.
  bool verify_readback{true};
  /// Publish the rollback guard every N commits; 0 disables the guard.
  std::uint64_t rollback_guard_every{64};
};

/// Result of opening and replaying a store.
struct CF_API RecoveryOutcome {
  CommitSequence last_commit{};
  std::uint64_t records_replayed{0};
  std::uint64_t discarded_tail_bytes{0};
  bool discarded_torn_tail{false};
  bool snapshot_loaded{false};
  CommitSequence snapshot_commit{};
  bool rollback_detected{false};
  bool created{false};
};

/// Exclusive writer authority for one domain store.
///
/// The lock is an OS-level exclusive handle on a canonical path. Two processes
/// cannot hold it at once, and a process death releases it.
class CF_API WriterLock {
 public:
  WriterLock() = default;
  WriterLock(WriterLock&& other) noexcept;
  WriterLock& operator=(WriterLock&& other) noexcept;
  WriterLock(const WriterLock&) = delete;
  WriterLock& operator=(const WriterLock&) = delete;
  ~WriterLock();

  /// Acquires the lock, creating \p lock_path if needed. Fails with StoreLocked
  /// when another process holds it, and with PathAmbiguous when the canonical
  /// path of the opened handle differs from the requested path.
  [[nodiscard]] static Result<WriterLock> acquire(const std::filesystem::path& lock_path);

  [[nodiscard]] bool held() const noexcept { return handle_ != nullptr; }
  [[nodiscard]] std::uint32_t writer_process_id() const noexcept { return process_id_; }
  [[nodiscard]] const Digest& incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] const std::string& canonical_path() const noexcept { return canonical_path_; }

  /// Releases the lock explicitly. Repeated calls are safe.
  void release() noexcept;

 private:
  void* handle_{nullptr};
  std::uint32_t process_id_{0};
  Digest incarnation_{};
  std::string canonical_path_{};
};

/// Validates, canonicalizes and creates a store root directory.
///
/// Rejects ambiguous roots (relative traversal, alternate separators, device
/// prefixes, reserved names, over-long components) and refuses to treat a
/// reparse point as a lock root.
[[nodiscard]] CF_API Result<std::filesystem::path> prepare_store_root(
    const std::filesystem::path& root);

/// Canonical directory of one domain inside a prepared root.
[[nodiscard]] CF_API std::filesystem::path domain_store_directory(
    const std::filesystem::path& prepared_root, FailoverDomainId domain);

/// Append-only, integrity-checked record log with atomic snapshot publication.
class CF_API DurableStore {
 public:
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  ~DurableStore();

  /// Opens (creating when missing) the store for \p domain and replays it.
  [[nodiscard]] static Result<std::unique_ptr<DurableStore>> open(const StoreOptions& options,
                                                                 FailoverDomainId domain);

  [[nodiscard]] const RecoveryOutcome& recovery() const noexcept { return recovery_; }
  [[nodiscard]] CommitSequence last_commit() const noexcept { return last_commit_; }
  [[nodiscard]] FailoverDomainId domain() const noexcept { return domain_; }
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
  [[nodiscard]] const Digest& store_identity() const noexcept { return store_identity_; }

  /// Records after the snapshot, in commit order, with their payloads.
  [[nodiscard]] const std::vector<std::pair<RecordType, std::vector<std::uint8_t>>>& records()
      const noexcept {
    return records_;
  }
  [[nodiscard]] std::span<const std::uint8_t> snapshot_payload() const noexcept {
    return snapshot_payload_;
  }
  [[nodiscard]] bool has_snapshot() const noexcept { return snapshot_loaded_; }

  /// Appends one record and commits it. The commit point is the flush plus the
  /// verified read-back.
  [[nodiscard]] Result<CommitSequence> append(RecordType type,
                                              std::span<const std::uint8_t> payload);

  /// Publishes \p payload as the complete state at the current commit sequence
  /// and starts a fresh WAL. Crash-safe in either ordering of the two renames.
  [[nodiscard]] Result<void> checkpoint(std::span<const std::uint8_t> payload);

  /// Highest commit sequence ever published, as recorded by the guard file.
  [[nodiscard]] Result<CommitSequence> read_rollback_guard() const;

 private:
  DurableStore() = default;

  [[nodiscard]] Result<void> finalize_open(const StoreOptions& options);
  [[nodiscard]] Result<void> write_all(const std::uint8_t* data, std::size_t size);
  [[nodiscard]] Result<void> flush();
  [[nodiscard]] Result<void> publish_guard();
  [[nodiscard]] Result<void> truncate_wal_to(std::uint64_t offset);

  StoreOptions options_{};
  FailoverDomainId domain_{};
  std::filesystem::path directory_{};
  WriterLock lock_{};
  std::FILE* wal_{nullptr};
  std::uint64_t wal_write_offset_{0};
  std::uint64_t wal_records_{0};
  CommitSequence last_commit_{};
  RecoveryOutcome recovery_{};
  Digest store_identity_{};
  bool snapshot_loaded_{false};
  std::vector<std::uint8_t> snapshot_payload_{};
  std::vector<std::pair<RecordType, std::vector<std::uint8_t>>> records_{};
  std::uint64_t commits_since_guard_{0};
};

/// Ordered, integrity-checked frame used for WAL records and snapshots.
struct CF_API RecordFrame {
  static constexpr std::size_t kHeaderBytes = 24;

  [[nodiscard]] static std::vector<std::uint8_t> encode(RecordType type, CommitSequence sequence,
                                                        std::span<const std::uint8_t> payload);
  /// Decodes a frame. Returns StoreCorrupt when the frame is not structurally
  /// valid, and StoreTruncated when the input ends inside the frame.
  [[nodiscard]] static Result<RecordFrame> decode(std::span<const std::uint8_t> bytes);

  RecordType type{RecordType::DomainRegistered};
  CommitSequence sequence{};
  std::vector<std::uint8_t> payload{};
  std::size_t frame_bytes{0};
};

}  // namespace cooling_failover
