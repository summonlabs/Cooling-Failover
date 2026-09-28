#include "store_internal.hpp"

#include "cooling_failover/version.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <process.h>
#else
#  include <ctime>
#  include <unistd.h>
#endif

namespace cooling_failover {
namespace detail {
namespace {

constexpr std::array<std::uint8_t, 8> kMetaMagic = {'C', 'F', 'F', 'A', 'I', 'L', 'O', 'V'};
constexpr std::array<std::uint8_t, 8> kWalMagic = {'C', 'F', 'F', 'A', 'I', 'L', 'W', 'L'};
constexpr std::array<std::uint8_t, 8> kSnapshotMagic = {'C', 'F', 'F', 'A', 'I', 'L', 'S', 'N'};
constexpr std::array<std::uint8_t, 8> kGuardMagic = {'C', 'F', 'F', 'A', 'I', 'L', 'G', 'D'};

void put_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
  out[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
  out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
  out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
}

void put_u64(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t value) {
  put_u32(out, offset, static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
  put_u32(out, offset + 4, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFULL));
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint64_t>(get_u32(bytes, offset)) |
         (static_cast<std::uint64_t>(get_u32(bytes, offset + 4)) << 32);
}

}  // namespace

Result<std::FILE*> open_file(const std::filesystem::path& path, const wchar_t* mode) {
  // A missing file and a file that exists but cannot be opened are different
  // failures: the first is StoreNotFound, the second is StoreIoError.
  const auto failure = [&path](std::int64_t error) {
    std::error_code code;
    const bool present = std::filesystem::exists(path, code) && !code;
    if (!present) {
      return Status::error(ErrorCode::StoreNotFound, "store file does not exist", "path",
                           path.string());
    }
    return Status::error(ErrorCode::StoreIoError, "store file could not be opened", "path",
                         path.string(), "error", error);
  };
#ifdef _WIN32
  std::FILE* file = nullptr;
  const errno_t outcome = ::_wfopen_s(&file, path.c_str(), mode);
  if (outcome != 0 || file == nullptr) {
    return failure(static_cast<std::int64_t>(outcome));
  }
  return file;
#else
  std::string narrow_mode;
  for (const wchar_t* cursor = mode; *cursor != L'\0'; ++cursor) {
    narrow_mode.push_back(static_cast<char>(*cursor));
  }
  const std::string narrow = path.string();
  std::FILE* file = std::fopen(narrow.c_str(), narrow_mode.c_str());
  if (file == nullptr) {
    return failure(static_cast<std::int64_t>(errno));
  }
  return file;
#endif
}

Result<std::FILE*> open_file_for_update(const std::filesystem::path& path) {
  return open_file(path, L"r+b");
}

Result<void> flush_stream(std::FILE* file) {
  if (std::fflush(file) != 0) {
    return Status::error(ErrorCode::StoreIoError, "flush failed", "errno",
                         static_cast<std::int64_t>(errno));
  }
#ifdef _WIN32
  if (::_commit(::_fileno(file)) != 0) {
    return Status::error(ErrorCode::StoreIoError, "flush-to-disk failed", "errno",
                         static_cast<std::int64_t>(errno));
  }
#else
  if (::fsync(::fileno(file)) != 0) {
    return Status::error(ErrorCode::StoreIoError, "flush-to-disk failed", "errno",
                         static_cast<std::int64_t>(errno));
  }
#endif
  return Result<void>();
}

Result<std::vector<std::uint8_t>> read_file_bytes(const std::filesystem::path& path) {
  Result<std::FILE*> opened = open_file(path, L"rb");
  if (!opened.has_value()) {
    return opened.status();
  }
  std::FILE* file = *opened;
  std::vector<std::uint8_t> bytes;
  std::array<std::uint8_t, 65536> buffer{};
  for (;;) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
    if (read > 0) {
      if (bytes.size() + read > kMaxStoreFileBytes) {
        std::fclose(file);
        return Status::error(ErrorCode::StoreLimitExceeded, "store file exceeds its size bound",
                             "path", path.string());
      }
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
    }
    if (read < buffer.size()) {
      break;
    }
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    return Status::error(ErrorCode::StoreIoError, "read failed", "path", path.string());
  }
  return bytes;
}

Result<std::vector<std::uint8_t>> read_stream_range(std::FILE* file, std::uint64_t offset,
                                                    std::size_t length) {
#ifdef _WIN32
  if (::_fseeki64(file, static_cast<__int64>(offset), SEEK_SET) != 0) {
#else
  if (::fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
    return Status::error(ErrorCode::StoreIoError, "seek failed during read-back verification");
  }
  std::vector<std::uint8_t> bytes(length);
  const std::size_t read = length == 0 ? 0 : std::fread(bytes.data(), 1, length, file);
  if (read != length) {
    return Status::error(ErrorCode::StoreTruncated, "short read during read-back verification",
                         "wanted", static_cast<std::uint64_t>(length), "read",
                         static_cast<std::uint64_t>(read));
  }
  return bytes;
}

Result<void> write_file_bytes(const std::filesystem::path& path,
                              std::span<const std::uint8_t> bytes) {
  Result<std::FILE*> opened = open_file(path, L"wb");
  if (!opened.has_value()) {
    return opened.status();
  }
  std::FILE* file = *opened;
  if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
    std::fclose(file);
    return Status::error(ErrorCode::StoreIoError, "write failed", "path", path.string());
  }
  Result<void> flushed = flush_stream(file);
  std::fclose(file);
  if (!flushed.has_value()) {
    return flushed.status();
  }
  return Result<void>();
}

Result<void> atomic_replace_path(const std::filesystem::path& source,
                                 const std::filesystem::path& target) {
#ifdef _WIN32
  if (::MoveFileExW(source.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status::error(ErrorCode::StoreIoError, "atomic replace failed", "target",
                         target.string(), "code", static_cast<std::uint64_t>(::GetLastError()));
  }
  return Result<void>();
#else
  std::error_code code;
  std::filesystem::rename(source, target, code);
  if (code) {
    return Status::error(ErrorCode::StoreIoError, "atomic replace failed", "target",
                         target.string(), "error", code.message());
  }
  return Result<void>();
#endif
}

bool is_reparse_point_path(const std::filesystem::path& path) {
#ifdef _WIN32
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  struct stat info {};
  if (::lstat(path.string().c_str(), &info) != 0) {
    return false;
  }
  return S_ISLNK(info.st_mode);
#endif
}

std::uint64_t process_identifier() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t process_start_marker() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetTickCount64());
#else
  return static_cast<std::uint64_t>(::time(nullptr));
#endif
}

std::filesystem::path temporary_path(const std::filesystem::path& directory,
                                     const wchar_t* tag) {
  static std::uint64_t counter = 0;
  ++counter;
  std::wstring name(tag);
  name += L".tmp-";
  name += std::to_wstring(process_identifier());
  name += L"-";
  name += std::to_wstring(counter);
  return directory / name;
}

void cleanup_temporary_files(const std::filesystem::path& directory) {
  std::error_code code;
  std::filesystem::directory_iterator iterator(directory, code);
  if (code) {
    return;
  }
  std::size_t inspected = 0;
  for (const std::filesystem::directory_entry& entry : iterator) {
    if (++inspected > 1024) {
      return;
    }
    const std::string name = entry.path().filename().string();
    if (name.find(".tmp-") == std::string::npos) {
      continue;
    }
    std::error_code remove_code;
    std::filesystem::remove(entry.path(), remove_code);
  }
}

std::vector<std::uint8_t> make_meta_bytes(FailoverDomainId domain, Tick created_at,
                                          const Digest& identity) {
  std::vector<std::uint8_t> meta(kMetaBytes, 0);
  std::copy(kMetaMagic.begin(), kMetaMagic.end(), meta.begin());
  put_u32(meta, 8, kStoreFormatVersion);
  put_u32(meta, 12, 0);
  put_u64(meta, 16, domain.value());
  put_u64(meta, 24, created_at.value());
  std::copy(identity.bytes().begin(), identity.bytes().end(), meta.begin() + 32);
  put_u32(meta, 60, crc32(std::span<const std::uint8_t>(meta.data(), 60)));
  return meta;
}

Result<Digest> parse_meta_bytes(std::span<const std::uint8_t> bytes, FailoverDomainId domain) {
  if (bytes.size() != kMetaBytes) {
    return Status::error(ErrorCode::StoreTruncated, "store metadata has the wrong size", "size",
                         static_cast<std::uint64_t>(bytes.size()));
  }
  if (!std::equal(kMetaMagic.begin(), kMetaMagic.end(), bytes.begin())) {
    return Status::error(ErrorCode::StoreCorrupt, "store metadata magic mismatch");
  }
  if (get_u32(bytes, 60) != crc32(bytes.subspan(0, 60))) {
    return Status::error(ErrorCode::StoreCorrupt, "store metadata integrity check failed");
  }
  const std::uint32_t version = get_u32(bytes, 8);
  if (version != kStoreFormatVersion) {
    return Status::error(ErrorCode::StoreVersionUnsupported,
                         "store metadata format version is not supported", "version",
                         static_cast<std::uint64_t>(version), "supported",
                         static_cast<std::uint64_t>(kStoreFormatVersion));
  }
  if (get_u32(bytes, 12) != 0) {
    return Status::error(ErrorCode::StoreReservedFieldSet, "store metadata reserved field is set");
  }
  if (get_u64(bytes, 16) != domain.value()) {
    return Status::error(ErrorCode::StoreCorrupt, "store metadata belongs to another domain",
                         "found", get_u64(bytes, 16), "expected", domain.value());
  }
  Digest identity;
  std::copy(bytes.begin() + 32, bytes.begin() + 64, identity.mutable_bytes().begin());
  return identity;
}

std::vector<std::uint8_t> make_wal_header_bytes(FailoverDomainId domain) {
  std::vector<std::uint8_t> header(kWalHeaderBytes, 0);
  std::copy(kWalMagic.begin(), kWalMagic.end(), header.begin());
  put_u32(header, 8, kStoreFormatVersion);
  put_u32(header, 12, 0);
  put_u64(header, 16, domain.value());
  put_u32(header, 24, crc32(std::span<const std::uint8_t>(header.data(), 24)));
  put_u32(header, 28, 0);
  return header;
}

Result<void> check_wal_header_bytes(std::span<const std::uint8_t> bytes,
                                    FailoverDomainId domain) {
  if (bytes.size() < kWalHeaderBytes) {
    return Status::error(ErrorCode::StoreTruncated, "write-ahead log header is incomplete");
  }
  if (!std::equal(kWalMagic.begin(), kWalMagic.end(), bytes.begin())) {
    return Status::error(ErrorCode::StoreCorrupt, "write-ahead log magic mismatch");
  }
  if (get_u32(bytes, 24) != crc32(bytes.subspan(0, 24))) {
    return Status::error(ErrorCode::StoreCorrupt, "write-ahead log header integrity check failed");
  }
  const std::uint32_t version = get_u32(bytes, 8);
  if (version != kStoreFormatVersion) {
    return Status::error(ErrorCode::StoreVersionUnsupported,
                         "write-ahead log format version is not supported", "version",
                         static_cast<std::uint64_t>(version));
  }
  if (get_u32(bytes, 12) != 0 || get_u32(bytes, 28) != 0) {
    return Status::error(ErrorCode::StoreReservedFieldSet, "write-ahead log reserved field is set");
  }
  if (get_u64(bytes, 16) != domain.value()) {
    return Status::error(ErrorCode::StoreCorrupt, "write-ahead log belongs to another domain");
  }
  return Result<void>();
}

std::vector<std::uint8_t> make_snapshot_bytes(FailoverDomainId domain, CommitSequence sequence,
                                              std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> bytes(kSnapshotHeaderBytes + payload.size(), 0);
  std::copy(kSnapshotMagic.begin(), kSnapshotMagic.end(), bytes.begin());
  put_u32(bytes, 8, kStoreFormatVersion);
  put_u32(bytes, 12, 0);
  put_u64(bytes, 16, domain.value());
  put_u64(bytes, 24, sequence.value());
  put_u32(bytes, 32, static_cast<std::uint32_t>(payload.size()));
  put_u32(bytes, 36, crc32(payload));
  put_u32(bytes, 40, crc32(std::span<const std::uint8_t>(bytes.data(), 40)));
  put_u32(bytes, 44, 0);
  std::copy(payload.begin(), payload.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(kSnapshotHeaderBytes));
  return bytes;
}

Result<SnapshotView> parse_snapshot_bytes(std::span<const std::uint8_t> bytes,
                                          FailoverDomainId domain) {
  if (bytes.size() < kSnapshotHeaderBytes) {
    return Status::error(ErrorCode::StoreTruncated, "snapshot header is incomplete");
  }
  if (!std::equal(kSnapshotMagic.begin(), kSnapshotMagic.end(), bytes.begin())) {
    return Status::error(ErrorCode::StoreCorrupt, "snapshot magic mismatch");
  }
  if (get_u32(bytes, 40) != crc32(bytes.subspan(0, 40))) {
    return Status::error(ErrorCode::StoreCorrupt, "snapshot header integrity check failed");
  }
  if (get_u32(bytes, 8) != kStoreFormatVersion) {
    return Status::error(ErrorCode::StoreVersionUnsupported, "snapshot format version unsupported");
  }
  if (get_u32(bytes, 12) != 0 || get_u32(bytes, 44) != 0) {
    return Status::error(ErrorCode::StoreReservedFieldSet, "snapshot reserved field is set");
  }
  if (get_u64(bytes, 16) != domain.value()) {
    return Status::error(ErrorCode::StoreCorrupt, "snapshot belongs to another domain");
  }
  const std::uint32_t declared = get_u32(bytes, 32);
  if (declared != bytes.size() - kSnapshotHeaderBytes) {
    return Status::error(ErrorCode::StoreCorrupt, "snapshot length does not match its payload",
                         "declared", static_cast<std::uint64_t>(declared), "actual",
                         static_cast<std::uint64_t>(bytes.size() - kSnapshotHeaderBytes));
  }
  if (crc32(bytes.subspan(kSnapshotHeaderBytes)) != get_u32(bytes, 36)) {
    return Status::error(ErrorCode::StoreCorrupt, "snapshot payload integrity check failed");
  }
  SnapshotView view;
  view.sequence = CommitSequence::from_value(get_u64(bytes, 24));
  view.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kSnapshotHeaderBytes), bytes.end());
  return view;
}

std::vector<std::uint8_t> make_guard_bytes(FailoverDomainId domain, CommitSequence sequence) {
  std::vector<std::uint8_t> bytes(kGuardBytes, 0);
  std::copy(kGuardMagic.begin(), kGuardMagic.end(), bytes.begin());
  put_u32(bytes, 8, kStoreFormatVersion);
  put_u32(bytes, 12, 0);
  put_u64(bytes, 16, sequence.value());
  put_u32(bytes, 24, crc32(std::span<const std::uint8_t>(bytes.data(), 24)));
  put_u32(bytes, 28, 0);
  (void)domain;
  return bytes;
}

Result<CommitSequence> parse_guard_bytes(std::span<const std::uint8_t> bytes,
                                         FailoverDomainId domain) {
  (void)domain;
  if (bytes.size() != kGuardBytes) {
    return Status::error(ErrorCode::StoreCorrupt, "rollback guard has the wrong size", "size",
                         static_cast<std::uint64_t>(bytes.size()));
  }
  if (!std::equal(kGuardMagic.begin(), kGuardMagic.end(), bytes.begin())) {
    return Status::error(ErrorCode::StoreCorrupt, "rollback guard magic mismatch");
  }
  if (get_u32(bytes, 24) != crc32(bytes.subspan(0, 24))) {
    return Status::error(ErrorCode::StoreCorrupt, "rollback guard integrity check failed");
  }
  if (get_u32(bytes, 8) != kStoreFormatVersion) {
    return Status::error(ErrorCode::StoreVersionUnsupported,
                         "rollback guard format version is unsupported");
  }
  if (get_u32(bytes, 12) != 0 || get_u32(bytes, 28) != 0) {
    return Status::error(ErrorCode::StoreReservedFieldSet, "rollback guard reserved field is set");
  }
  return CommitSequence::from_value(get_u64(bytes, 16));
}

Result<WalScan> scan_wal(std::span<const std::uint8_t> bytes, FailoverDomainId domain,
                         std::uint32_t max_record_bytes) {
  CF_TRY(check_wal_header_bytes(bytes, domain));
  WalScan scan;
  scan.valid_end = kWalHeaderBytes;
  std::uint64_t offset = kWalHeaderBytes;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - static_cast<std::size_t>(offset);
    Result<RecordFrame> frame =
        RecordFrame::decode(bytes.subspan(static_cast<std::size_t>(offset), remaining));
    if (!frame.has_value()) {
      // The first unreadable record is the logical end of the log: an
      // interrupted append. Nothing after it is ever considered.
      scan.tore = true;
      scan.discarded_tail_bytes = remaining;
      break;
    }
    if (frame->payload.size() > max_record_bytes) {
      return Status::error(ErrorCode::StoreLimitExceeded,
                           "write-ahead log record exceeds the configured bound", "bytes",
                           static_cast<std::uint64_t>(frame->payload.size()));
    }
    const std::uint64_t expected =
        scan.records.empty() ? frame->sequence.value() : scan.last_sequence + 1;
    if (frame->sequence.value() != expected) {
      return Status::error(ErrorCode::StoreCorrupt,
                           "write-ahead log commit sequence is not contiguous", "expected", expected,
                           "found", frame->sequence.value());
    }
    if (scan.records.empty()) {
      scan.first_sequence = frame->sequence.value();
    }
    scan.last_sequence = frame->sequence.value();
    offset += frame->frame_bytes;
    scan.valid_end = offset;
    scan.records.emplace_back(frame->type, std::move(frame->payload));
  }
  return scan;
}

}  // namespace detail
}  // namespace cooling_failover
