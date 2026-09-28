#include "cooling_failover/store.hpp"

#include "cooling_failover/version.hpp"
#include "store_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace cooling_failover {
namespace {

constexpr const wchar_t* kWalName = L"wal.log";
constexpr const wchar_t* kSnapshotName = L"snapshot.dat";
constexpr const wchar_t* kMetaName = L"store.meta";
constexpr const wchar_t* kGuardName = L"rollback.guard";
constexpr const wchar_t* kLockName = L"writer.lock";

}  // namespace

Result<std::unique_ptr<DurableStore>> DurableStore::open(const StoreOptions& options,
                                                         FailoverDomainId domain) {
  if (domain.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "failover domain identity is nil");
  }
  if (options.max_record_bytes == 0 || options.max_record_bytes > kMaxBlobBytes) {
    return Status::error(ErrorCode::OutOfRange, "max_record_bytes is outside the supported range",
                         "value", static_cast<std::uint64_t>(options.max_record_bytes));
  }

  CF_TRY_ASSIGN(prepared, prepare_store_root(options.root));
  const std::filesystem::path directory = domain_store_directory(prepared, domain);

  std::error_code code;
  std::filesystem::create_directories(directory, code);
  if (code) {
    return Status::error(ErrorCode::StoreIoError, "domain store directory could not be created",
                         "error", code.message());
  }
  if (detail::is_reparse_point_path(directory)) {
    return Status::error(ErrorCode::PathReparsePoint, "domain store directory is a reparse point",
                         "path", directory.string());
  }

  CF_TRY_ASSIGN(lock, WriterLock::acquire(directory / kLockName));

  auto store = std::unique_ptr<DurableStore>(new DurableStore());
  store->options_ = options;
  store->domain_ = domain;
  store->directory_ = directory;
  store->lock_ = std::move(lock);
  CF_TRY(store->finalize_open(options));
  return store;
}

Result<void> DurableStore::finalize_open(const StoreOptions& options) {
  detail::cleanup_temporary_files(directory_);

  // ---- store metadata ----------------------------------------------------
  const std::filesystem::path meta_path = directory_ / kMetaName;
  if (std::filesystem::exists(meta_path)) {
    CF_TRY_ASSIGN(bytes, detail::read_file_bytes(meta_path));
    CF_TRY_ASSIGN(identity, detail::parse_meta_bytes(bytes, domain_));
    store_identity_ = identity;
  } else {
    Encoder encoder;
    encoder.u32(kStoreFormatVersion);
    encoder.id(domain_);
    encoder.text(directory_.string());
    encoder.u64(detail::process_identifier());
    encoder.u64(detail::process_start_marker());
    const Digest identity = encoder.finish_digest();
    const std::vector<std::uint8_t> meta =
        detail::make_meta_bytes(domain_, Tick::from_value(0), identity);
    CF_TRY(detail::write_file_bytes(meta_path, meta));
    CF_TRY_ASSIGN(readback, detail::read_file_bytes(meta_path));
    if (readback != meta) {
      return Status::error(ErrorCode::StoreIoError, "store metadata read-back mismatch");
    }
    store_identity_ = identity;
    recovery_.created = true;
  }

  // ---- snapshot ----------------------------------------------------------
  const std::filesystem::path snapshot_path = directory_ / kSnapshotName;
  CommitSequence snapshot_commit{};
  if (std::filesystem::exists(snapshot_path)) {
    CF_TRY_ASSIGN(bytes, detail::read_file_bytes(snapshot_path));
    CF_TRY_ASSIGN(view, detail::parse_snapshot_bytes(bytes, domain_));
    snapshot_commit = view.sequence;
    snapshot_payload_ = std::move(view.payload);
    snapshot_loaded_ = true;
    recovery_.snapshot_loaded = true;
    recovery_.snapshot_commit = snapshot_commit;
  }

  // ---- write-ahead log ---------------------------------------------------
  const std::filesystem::path wal_path = directory_ / kWalName;
  if (!std::filesystem::exists(wal_path)) {
    const std::vector<std::uint8_t> header = detail::make_wal_header_bytes(domain_);
    CF_TRY(detail::write_file_bytes(wal_path, header));
    CF_TRY_ASSIGN(readback, detail::read_file_bytes(wal_path));
    if (readback != header) {
      return Status::error(ErrorCode::StoreIoError, "write-ahead log read-back mismatch");
    }
  }

  detail::WalScan scan;
  {
    CF_TRY_ASSIGN(bytes, detail::read_file_bytes(wal_path));
    CF_TRY_ASSIGN(parsed, detail::scan_wal(bytes, domain_, options.max_record_bytes));
    scan = std::move(parsed);
  }

  if (scan.discarded_tail_bytes >
      static_cast<std::uint64_t>(options.max_record_bytes) + RecordFrame::kHeaderBytes) {
    return Status::error(ErrorCode::StoreCorrupt, "write-ahead log has an oversized unreadable tail",
                         "bytes", scan.discarded_tail_bytes);
  }

  // Records at or below the snapshot commit were superseded by the snapshot.
  std::uint64_t kept_first = 0;
  std::uint64_t kept_last = snapshot_commit.value();
  std::uint64_t sequence = scan.first_sequence;
  for (auto& record : scan.records) {
    const std::uint64_t this_sequence = sequence++;
    if (this_sequence <= snapshot_commit.value()) {
      continue;
    }
    if (records_.empty()) {
      kept_first = this_sequence;
    }
    kept_last = this_sequence;
    records_.push_back(std::move(record));
  }

  if (!records_.empty() && kept_first != snapshot_commit.value() + 1) {
    return Status::error(ErrorCode::StoreCorrupt,
                         "write-ahead log does not continue from the snapshot commit", "expected",
                         snapshot_commit.value() + 1, "found", kept_first);
  }

  recovery_.records_replayed = records_.size();
  recovery_.discarded_tail_bytes = scan.discarded_tail_bytes;
  recovery_.discarded_torn_tail = scan.tore;
  last_commit_ = CommitSequence::from_value(kept_last);
  recovery_.last_commit = last_commit_;

  // ---- rollback guard ----------------------------------------------------
  if (options.rollback_guard_every != 0) {
    const std::filesystem::path guard_path = directory_ / kGuardName;
    if (std::filesystem::exists(guard_path)) {
      CF_TRY_ASSIGN(bytes, detail::read_file_bytes(guard_path));
      CF_TRY_ASSIGN(guard, detail::parse_guard_bytes(bytes, domain_));
      if (guard.value() > last_commit_.value()) {
        recovery_.rollback_detected = true;
        return Status::error(ErrorCode::StoreRolledBack,
                             "the store has rolled back below its published commit sequence",
                             "guard", guard.value(), "resolved", last_commit_.value());
      }
    }
  }

  // ---- open for appending ------------------------------------------------
  CF_TRY_ASSIGN(wal_file, detail::open_file_for_update(wal_path));
  wal_ = wal_file;
  wal_write_offset_ = scan.valid_end;
  wal_records_ = records_.size();

  if (scan.tore && scan.discarded_tail_bytes > 0) {
    CF_TRY(truncate_wal_to(scan.valid_end));
  }

#ifdef _WIN32
  if (::_fseeki64(wal_, static_cast<__int64>(wal_write_offset_), SEEK_SET) != 0) {
#else
  if (::fseeko(wal_, static_cast<off_t>(wal_write_offset_), SEEK_SET) != 0) {
#endif
    return Status::error(ErrorCode::StoreIoError, "write-ahead log could not be positioned");
  }
  CF_TRY(publish_guard());
  return Result<void>();
}

Result<CommitSequence> DurableStore::append(RecordType type, std::span<const std::uint8_t> payload) {
  if (wal_ == nullptr) {
    return Status::error(ErrorCode::StoreClosed, "store is not open for appending");
  }
  if (payload.size() > options_.max_record_bytes) {
    return Status::error(ErrorCode::BoundsExceeded, "record payload exceeds the configured bound",
                         "bytes", static_cast<std::uint64_t>(payload.size()));
  }
  if (wal_records_ >= options_.max_records) {
    return Status::error(ErrorCode::StoreLimitExceeded,
                         "write-ahead log reached its record bound", "records", wal_records_);
  }

  CF_TRY_ASSIGN(sequence, last_commit_.next());
  const std::vector<std::uint8_t> frame = RecordFrame::encode(type, sequence, payload);
  const std::uint64_t offset = wal_write_offset_;

  if (std::fwrite(frame.data(), 1, frame.size(), wal_) != frame.size()) {
    return Status::error(ErrorCode::StoreIoError, "record write failed");
  }
  if (options_.flush_on_commit) {
    CF_TRY(detail::flush_stream(wal_));
  } else if (std::fflush(wal_) != 0) {
    return Status::error(ErrorCode::StoreIoError, "record flush failed");
  }

  if (options_.verify_readback) {
    CF_TRY_ASSIGN(readback, detail::read_stream_range(wal_, offset, frame.size()));
    if (readback != frame) {
      return Status::error(ErrorCode::StoreIoError, "record read-back verification failed",
                           "offset", offset);
    }
  }

  // Commit point: the record is durable and its bytes read back identically.
  last_commit_ = sequence;
  wal_write_offset_ = offset + frame.size();
  ++wal_records_;
  ++commits_since_guard_;
  if (options_.rollback_guard_every != 0 &&
      commits_since_guard_ >= options_.rollback_guard_every) {
    CF_TRY(publish_guard());
  }
  return last_commit_;
}

Result<void> DurableStore::checkpoint(std::span<const std::uint8_t> payload) {
  if (wal_ == nullptr) {
    return Status::error(ErrorCode::StoreClosed, "store is not open");
  }
  if (payload.size() > options_.max_record_bytes) {
    return Status::error(ErrorCode::BoundsExceeded, "snapshot payload exceeds the configured bound");
  }

  const std::vector<std::uint8_t> snapshot_bytes =
      detail::make_snapshot_bytes(domain_, last_commit_, payload);
  const std::filesystem::path snapshot_path = directory_ / kSnapshotName;
  const std::filesystem::path snapshot_temp = detail::temporary_path(directory_, L"snapshot");

  // The write-ahead log must not be open while its file is replaced.
  std::fclose(wal_);
  wal_ = nullptr;

  CF_TRY(detail::write_file_bytes(snapshot_temp, snapshot_bytes));
  CF_TRY_ASSIGN(readback, detail::read_file_bytes(snapshot_temp));
  if (readback != snapshot_bytes) {
    return Status::error(ErrorCode::StoreIoError, "snapshot read-back verification failed");
  }
  // Publication point of the state snapshot.
  CF_TRY(detail::atomic_replace_path(snapshot_temp, snapshot_path));

  const std::filesystem::path wal_path = directory_ / kWalName;
  const std::filesystem::path wal_temp = detail::temporary_path(directory_, L"wal");
  CF_TRY(detail::write_file_bytes(wal_temp, detail::make_wal_header_bytes(domain_)));
  CF_TRY(detail::atomic_replace_path(wal_temp, wal_path));

  CF_TRY_ASSIGN(wal_file, detail::open_file_for_update(wal_path));
  wal_ = wal_file;
#ifdef _WIN32
  if (::_fseeki64(wal_, 0, SEEK_END) != 0) {
#else
  if (::fseeko(wal_, 0, SEEK_END) != 0) {
#endif
    return Status::error(ErrorCode::StoreIoError, "write-ahead log could not be positioned");
  }
  wal_write_offset_ = detail::kWalHeaderBytes;
  wal_records_ = 0;
  snapshot_payload_.assign(payload.begin(), payload.end());
  snapshot_loaded_ = true;
  recovery_.snapshot_commit = last_commit_;
  recovery_.snapshot_loaded = true;
  commits_since_guard_ = 0;
  CF_TRY(publish_guard());
  return Result<void>();
}

Result<CommitSequence> DurableStore::read_rollback_guard() const {
  const std::filesystem::path guard_path = directory_ / kGuardName;
  if (!std::filesystem::exists(guard_path)) {
    return Status::error(ErrorCode::StoreNotFound, "no rollback guard has been published");
  }
  CF_TRY_ASSIGN(bytes, detail::read_file_bytes(guard_path));
  CF_TRY_ASSIGN(guard, detail::parse_guard_bytes(bytes, domain_));
  return guard;
}

Result<void> DurableStore::publish_guard() {
  if (options_.rollback_guard_every == 0) {
    return Result<void>();
  }
  const std::filesystem::path guard_path = directory_ / kGuardName;
  const std::filesystem::path guard_temp = detail::temporary_path(directory_, L"guard");
  CF_TRY(detail::write_file_bytes(guard_temp, detail::make_guard_bytes(domain_, last_commit_)));
  CF_TRY(detail::atomic_replace_path(guard_temp, guard_path));
  commits_since_guard_ = 0;
  return Result<void>();
}

Result<void> DurableStore::truncate_wal_to(std::uint64_t offset) {
  if (wal_ == nullptr) {
    return Status::error(ErrorCode::StoreClosed, "store is not open");
  }
  if (std::fflush(wal_) != 0) {
    return Status::error(ErrorCode::StoreIoError, "write-ahead log flush failed before truncation");
  }
#ifdef _WIN32
  if (::_chsize_s(::_fileno(wal_), static_cast<__int64>(offset)) != 0) {
#else
  if (::ftruncate(::fileno(wal_), static_cast<off_t>(offset)) != 0) {
#endif
    return Status::error(ErrorCode::StoreIoError, "write-ahead log could not be truncated");
  }
  return detail::flush_stream(wal_);
}

DurableStore::~DurableStore() {
  if (wal_ != nullptr) {
    std::fflush(wal_);
    std::fclose(wal_);
    wal_ = nullptr;
    // A graceful close publishes the true commit sequence so that a later
    // rollback of the log below it is detected rather than accepted.
    (void)publish_guard();
  }
  lock_.release();
}

}  // namespace cooling_failover
