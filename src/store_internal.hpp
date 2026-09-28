// Cooling Failover - internal store helpers.
//
// Not installed. Shared by the store translation units only.
#pragma once

#include "cooling_failover/canonical.hpp"
#include "cooling_failover/ids.hpp"
#include "cooling_failover/status.hpp"
#include "cooling_failover/store.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace cooling_failover {
namespace detail {

inline constexpr std::size_t kMetaBytes = 64;
inline constexpr std::size_t kWalHeaderBytes = 32;
inline constexpr std::size_t kSnapshotHeaderBytes = 48;
inline constexpr std::size_t kGuardBytes = 32;
inline constexpr std::size_t kMaxStoreFileBytes = 512U * 1024U * 1024U;

struct SnapshotView {
  CommitSequence sequence{};
  std::vector<std::uint8_t> payload{};
};

/// Bounded scan result over one write-ahead log.
struct WalScan {
  std::uint64_t valid_end{0};
  std::uint64_t first_sequence{0};
  std::uint64_t last_sequence{0};
  std::uint64_t discarded_tail_bytes{0};
  bool tore{false};
  std::vector<std::pair<RecordType, std::vector<std::uint8_t>>> records{};
};

[[nodiscard]] Result<std::FILE*> open_file(const std::filesystem::path& path, const wchar_t* mode);
[[nodiscard]] Result<std::FILE*> open_file_for_update(const std::filesystem::path& path);
[[nodiscard]] Result<void> flush_stream(std::FILE* file);
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file_bytes(const std::filesystem::path& path);
[[nodiscard]] Result<std::vector<std::uint8_t>> read_stream_range(std::FILE* file,
                                                                 std::uint64_t offset,
                                                                 std::size_t length);
[[nodiscard]] Result<void> write_file_bytes(const std::filesystem::path& path,
                                            std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<void> atomic_replace_path(const std::filesystem::path& source,
                                               const std::filesystem::path& target);
[[nodiscard]] bool is_reparse_point_path(const std::filesystem::path& path);
[[nodiscard]] std::uint64_t process_identifier() noexcept;
[[nodiscard]] std::uint64_t process_start_marker() noexcept;
[[nodiscard]] std::filesystem::path temporary_path(const std::filesystem::path& directory,
                                                   const wchar_t* tag);
void cleanup_temporary_files(const std::filesystem::path& directory);

[[nodiscard]] std::vector<std::uint8_t> make_meta_bytes(FailoverDomainId domain, Tick created_at,
                                                        const Digest& identity);
[[nodiscard]] Result<Digest> parse_meta_bytes(std::span<const std::uint8_t> bytes,
                                              FailoverDomainId domain);
[[nodiscard]] std::vector<std::uint8_t> make_wal_header_bytes(FailoverDomainId domain);
[[nodiscard]] Result<void> check_wal_header_bytes(std::span<const std::uint8_t> bytes,
                                                  FailoverDomainId domain);
[[nodiscard]] std::vector<std::uint8_t> make_snapshot_bytes(FailoverDomainId domain,
                                                            CommitSequence sequence,
                                                            std::span<const std::uint8_t> payload);
[[nodiscard]] Result<SnapshotView> parse_snapshot_bytes(std::span<const std::uint8_t> bytes,
                                                        FailoverDomainId domain);
[[nodiscard]] std::vector<std::uint8_t> make_guard_bytes(FailoverDomainId domain,
                                                         CommitSequence sequence);
[[nodiscard]] Result<CommitSequence> parse_guard_bytes(std::span<const std::uint8_t> bytes,
                                                       FailoverDomainId domain);
[[nodiscard]] Result<WalScan> scan_wal(std::span<const std::uint8_t> bytes, FailoverDomainId domain,
                                       std::uint32_t max_record_bytes);

}  // namespace detail
}  // namespace cooling_failover
