// Cooling Failover - canonical binary encoding, digests and integrity checks.
//
// The encoding is little-endian, length-prefixed and strictly bounded. It is
// canonical: two structurally equal values always produce identical bytes.
#pragma once

#include "cooling_failover/export.hpp"
#include "cooling_failover/ids.hpp"
#include "cooling_failover/status.hpp"
#include "cooling_failover/units.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cooling_failover {

/// Hard bounds applied to every decoded field. Nothing is allocated from a
/// declared size without first checking it against these limits and against the
/// number of bytes actually available.
inline constexpr std::size_t kMaxTextFieldBytes = 4096;
inline constexpr std::size_t kMaxBlobBytes = 1U << 20;  // 1 MiB
inline constexpr std::size_t kMaxCollectionItems = 65536;

/// SHA-256 digest, used as the deterministic identity of an encoded value.
class [[nodiscard]] CF_API Digest {
 public:
  static constexpr std::size_t kBytes = 32;

  Digest() noexcept = default;

  [[nodiscard]] static Digest of(std::span<const std::uint8_t> bytes) noexcept;
  [[nodiscard]] static Result<Digest> from_hex(std::string_view text);

  /// Lower-case hex. The all-zero digest renders as 64 zeros.
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] bool is_zero() const noexcept;

  [[nodiscard]] const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::array<std::uint8_t, kBytes>& mutable_bytes() noexcept { return bytes_; }

  friend bool operator==(const Digest& a, const Digest& b) noexcept { return a.bytes_ == b.bytes_; }
  friend bool operator!=(const Digest& a, const Digest& b) noexcept { return a.bytes_ != b.bytes_; }
  friend auto operator<=>(const Digest& a, const Digest& b) noexcept { return a.bytes_ <=> b.bytes_; }

 private:
  std::array<std::uint8_t, kBytes> bytes_{};
};

/// SHA-256 over an arbitrary byte range.
[[nodiscard]] CF_API Digest sha256(std::span<const std::uint8_t> bytes) noexcept;

/// Incremental SHA-256.
class [[nodiscard]] CF_API Sha256 {
 public:
  Sha256() noexcept;
  void update(std::span<const std::uint8_t> bytes) noexcept;
  [[nodiscard]] Digest finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_{0};
  std::size_t buffered_{0};
};

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
[[nodiscard]] CF_API std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] CF_API std::uint32_t crc32_extend(std::uint32_t seed,
                                                std::span<const std::uint8_t> bytes) noexcept;

/// Canonical encoder.
class [[nodiscard]] CF_API Encoder {
 public:
  Encoder() = default;

  void u8(std::uint8_t value) noexcept;
  void u16(std::uint16_t value) noexcept;
  void u32(std::uint32_t value) noexcept;
  void u64(std::uint64_t value) noexcept;
  void i64(std::int64_t value) noexcept;
  void boolean(bool value) noexcept;

  /// u32 length prefix followed by the raw bytes. Length is checked against
  /// kMaxTextFieldBytes.
  void text(std::string_view value);
  /// u32 length prefix followed by raw bytes; checked against kMaxBlobBytes.
  void blob(std::span<const std::uint8_t> value);
  /// Item count prefix; checked against kMaxCollectionItems.
  void count(std::size_t items);

  template <class Tag>
  void id(Id<Tag> value) noexcept {
    u64(value.value());
  }
  template <class Tag>
  void generation(Generation<Tag> value) noexcept {
    u64(value.value());
  }
  template <class Tag>
  void counter(Counter<Tag> value) noexcept {
    u64(value.value());
  }

  void tick(Tick value) noexcept { u64(value.value()); }
  void watts(Watts value) noexcept { i64(value.value()); }
  void basis_points(BasisPoints value) noexcept { u32(static_cast<std::uint32_t>(value.value())); }
  void digest(const Digest& value) noexcept;

  /// Item count derived from a container size.
  template <class Container>
  void collection(const Container& items) {
    count(items.size());
  }

  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(buffer_); }
  [[nodiscard]] Digest finish_digest() const noexcept {
    return sha256(std::span<const std::uint8_t>(buffer_.data(), buffer_.size()));
  }

  /// Encoders are infallible for in-range values; a bound violation latches a
  /// status and stops writing. Callers that build encodings from external data
  /// must check ok() before using data().
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  void fail(ErrorCode code, std::string message) noexcept;

 private:
  std::vector<std::uint8_t> buffer_{};
  Status status_{};
};

/// Strict canonical decoder. Errors are sticky: the first failure wins and all
/// subsequent reads return zero values without touching the input.
class [[nodiscard]] CF_API Decoder {
 public:
  explicit Decoder(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  std::uint8_t u8() noexcept;
  std::uint16_t u16() noexcept;
  std::uint32_t u32() noexcept;
  std::uint64_t u64() noexcept;
  std::int64_t i64() noexcept;
  bool boolean() noexcept;

  std::string text(std::size_t max_bytes = kMaxTextFieldBytes);
  std::vector<std::uint8_t> blob(std::size_t max_bytes = kMaxBlobBytes);
  /// Reads an item count; fails when it exceeds \p max_items or the remaining
  /// byte count (each item needs at least one byte).
  std::size_t count(std::size_t max_items = kMaxCollectionItems) noexcept;
  /// Reads a count that must equal \p expected.
  bool count_equals(std::size_t expected, std::string_view what) noexcept;

  template <class Tag>
  Id<Tag> id() noexcept {
    return Id<Tag>::from_value(u64());
  }
  template <class Tag>
  Generation<Tag> generation() noexcept {
    return Generation<Tag>::from_value(u64());
  }
  template <class Tag>
  Counter<Tag> counter() noexcept {
    return Counter<Tag>::from_value(u64());
  }

  Tick tick() noexcept { return Tick::from_value(u64()); }
  Watts watts() noexcept { return Watts::from_watts(i64()); }
  Result<BasisPoints> basis_points() noexcept;
  Digest digest() noexcept;
  /// Reads a boolean-encoded enumeration and validates the range [1, max_value].
  template <class Enum>
  Enum enumeration(std::uint8_t max_value, std::string_view what) noexcept {
    const std::uint8_t raw = u8();
    if (!ok()) {
      return static_cast<Enum>(1);
    }
    if (raw < 1 || raw > max_value) {
      fail(ErrorCode::InvalidEnumValue, std::string("enumeration out of range: ") + std::string(what));
      return static_cast<Enum>(1);
    }
    return static_cast<Enum>(raw);
  }

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] bool at_end() const noexcept { return pos_ == data_.size(); }

  /// Fails with StoreTrailingBytes when bytes remain.
  [[nodiscard]] Result<void> require_end(std::string_view what) const;
  void fail(ErrorCode code, std::string message) noexcept;

  /// Reserved fields must decode to their documented value.
  bool reserved_zero(std::string_view what) noexcept;
  bool reserved_u32(std::uint32_t expected, std::string_view what) noexcept;

 private:
  bool need(std::size_t bytes) noexcept;

  std::span<const std::uint8_t> data_{};
  std::size_t pos_{0};
  Status status_{};
};

[[nodiscard]] CF_API std::string to_hex(std::span<const std::uint8_t> bytes);
[[nodiscard]] CF_API Result<std::vector<std::uint8_t>> from_hex(std::string_view text);

}  // namespace cooling_failover
