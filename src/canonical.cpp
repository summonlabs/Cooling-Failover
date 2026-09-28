#include "cooling_failover/canonical.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace cooling_failover {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32U - bits));
}

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256U; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = ((value & 1U) != 0U) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32Table = make_crc32_table();

}  // namespace

Digest sha256(std::span<const std::uint8_t> bytes) noexcept {
  Sha256 hasher;
  hasher.update(bytes);
  return hasher.finish();
}

Sha256::Sha256() noexcept
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
             0x1f83d9abU, 0x5be0cd19U} {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64] = {};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                      (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                      static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3);
    const std::uint32_t s1 =
        rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256Constants[index] + schedule[index];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> bytes) noexcept {
  total_bytes_ += static_cast<std::uint64_t>(bytes.size());
  std::size_t offset = 0;
  if (buffered_ != 0) {
    const std::size_t wanted = 64 - buffered_;
    const std::size_t take = std::min(wanted, bytes.size());
    std::memcpy(buffer_.data() + buffered_, bytes.data(), take);
    buffered_ += take;
    offset = take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (bytes.size() - offset >= 64) {
    compress(bytes.data() + offset);
    offset += 64;
  }
  if (offset < bytes.size()) {
    const std::size_t tail = bytes.size() - offset;
    std::memcpy(buffer_.data(), bytes.data() + offset, tail);
    buffered_ = tail;
  }
}

Digest Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8U;
  std::array<std::uint8_t, 8> length_bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[index] = static_cast<std::uint8_t>((bit_length >> (56U - 8U * index)) & 0xFFU);
  }

  std::array<std::uint8_t, 72> padding{};
  padding[0] = 0x80U;
  const std::size_t pad_length = (buffered_ < 56) ? (56 - buffered_) : (120 - buffered_);
  update(std::span<const std::uint8_t>(padding.data(), pad_length));
  update(std::span<const std::uint8_t>(length_bytes.data(), length_bytes.size()));

  Digest digest;
  std::array<std::uint8_t, Digest::kBytes>& out = digest.mutable_bytes();
  for (std::size_t index = 0; index < 8; ++index) {
    out[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24) & 0xFFU);
    out[index * 4 + 1] = static_cast<std::uint8_t>((state_[index] >> 16) & 0xFFU);
    out[index * 4 + 2] = static_cast<std::uint8_t>((state_[index] >> 8) & 0xFFU);
    out[index * 4 + 3] = static_cast<std::uint8_t>(state_[index] & 0xFFU);
  }
  // The length field was fed through update(), which advanced total_bytes_;
  // reset so the object is inert after finish().
  total_bytes_ = 0;
  buffered_ = 0;
  return digest;
}

Digest Digest::of(std::span<const std::uint8_t> bytes) noexcept { return sha256(bytes); }

Result<Digest> Digest::from_hex(std::string_view text) {
  if (text.size() != kBytes * 2) {
    return Status::error(ErrorCode::MalformedIdentity, "digest hex must be 64 characters", "length",
                         static_cast<std::uint64_t>(text.size()));
  }
  Digest digest;
  for (std::size_t index = 0; index < kBytes; ++index) {
    std::uint32_t value = 0;
    for (std::size_t half = 0; half < 2; ++half) {
      const char c = text[index * 2 + half];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a') + 10U;
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A') + 10U;
      } else {
        return Status::error(ErrorCode::MalformedIdentity, "digest hex contains a non-hex character",
                             "index", static_cast<std::uint64_t>(index * 2 + half));
      }
      value = (value << 4) | digit;
    }
    digest.mutable_bytes()[index] = static_cast<std::uint8_t>(value);
  }
  return digest;
}

std::string Digest::to_hex() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(kBytes * 2);
  for (std::uint8_t byte : bytes_) {
    out.push_back(kDigits[(byte >> 4) & 0x0FU]);
    out.push_back(kDigits[byte & 0x0FU]);
  }
  return out;
}

bool Digest::is_zero() const noexcept {
  for (std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::uint32_t crc32_extend(std::uint32_t seed, std::span<const std::uint8_t> bytes) noexcept {
  std::uint32_t value = seed ^ 0xFFFFFFFFU;
  for (std::uint8_t byte : bytes) {
    value = kCrc32Table[(value ^ byte) & 0xFFU] ^ (value >> 8);
  }
  return value ^ 0xFFFFFFFFU;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
  return crc32_extend(0U, bytes);
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

void Encoder::fail(ErrorCode code, std::string message) noexcept {
  if (status_.ok()) {
    status_ = Status::error(code, std::move(message));
  }
}

void Encoder::u8(std::uint8_t value) noexcept {
  if (!status_.ok()) {
    return;
  }
  buffer_.push_back(value);
}

void Encoder::u16(std::uint16_t value) noexcept {
  u8(static_cast<std::uint8_t>(value & 0xFFU));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void Encoder::u32(std::uint32_t value) noexcept {
  u16(static_cast<std::uint16_t>(value & 0xFFFFU));
  u16(static_cast<std::uint16_t>((value >> 16) & 0xFFFFU));
}

void Encoder::u64(std::uint64_t value) noexcept {
  u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
  u32(static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFULL));
}

void Encoder::i64(std::int64_t value) noexcept { u64(static_cast<std::uint64_t>(value)); }

void Encoder::boolean(bool value) noexcept { u8(value ? 1U : 0U); }

void Encoder::text(std::string_view value) {
  if (!status_.ok()) {
    return;
  }
  if (value.size() > kMaxTextFieldBytes) {
    fail(ErrorCode::BoundsExceeded, "text field exceeds the maximum encoded length");
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  if (!status_.ok()) {
    return;
  }
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void Encoder::blob(std::span<const std::uint8_t> value) {
  if (!status_.ok()) {
    return;
  }
  if (value.size() > kMaxBlobBytes) {
    fail(ErrorCode::BoundsExceeded, "blob exceeds the maximum encoded length");
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  if (!status_.ok()) {
    return;
  }
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void Encoder::count(std::size_t items) {
  if (!status_.ok()) {
    return;
  }
  if (items > kMaxCollectionItems) {
    fail(ErrorCode::BoundsExceeded, "collection exceeds the maximum encoded item count");
    return;
  }
  u32(static_cast<std::uint32_t>(items));
}

void Encoder::digest(const Digest& value) noexcept {
  if (!status_.ok()) {
    return;
  }
  buffer_.insert(buffer_.end(), value.bytes().begin(), value.bytes().end());
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

void Decoder::fail(ErrorCode code, std::string message) noexcept {
  if (status_.ok()) {
    status_ = Status::error(code, std::move(message));
  }
}

bool Decoder::need(std::size_t bytes) noexcept {
  if (!status_.ok()) {
    return false;
  }
  if (bytes > data_.size() - pos_) {
    fail(ErrorCode::StoreTruncated, "encoded input ended before the value was complete");
    return false;
  }
  return true;
}

std::uint8_t Decoder::u8() noexcept {
  if (!need(1)) {
    return 0;
  }
  return data_[pos_++];
}

std::uint16_t Decoder::u16() noexcept {
  const std::uint16_t low = u8();
  const std::uint16_t high = u8();
  return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
}

std::uint32_t Decoder::u32() noexcept {
  const std::uint32_t low = u16();
  const std::uint32_t high = u16();
  return low | (high << 16);
}

std::uint64_t Decoder::u64() noexcept {
  const std::uint64_t low = u32();
  const std::uint64_t high = u32();
  return low | (high << 32);
}

std::int64_t Decoder::i64() noexcept { return static_cast<std::int64_t>(u64()); }

bool Decoder::boolean() noexcept {
  const std::uint8_t raw = u8();
  if (!status_.ok()) {
    return false;
  }
  if (raw > 1U) {
    fail(ErrorCode::InvalidEnumValue, "boolean field is neither 0 nor 1");
    return false;
  }
  return raw == 1U;
}

std::string Decoder::text(std::size_t max_bytes) {
  if (!status_.ok()) {
    return {};
  }
  const std::uint32_t declared = u32();
  if (!status_.ok()) {
    return {};
  }
  const std::size_t limit = std::min(max_bytes, kMaxTextFieldBytes);
  if (declared > limit) {
    fail(ErrorCode::BoundsExceeded, "decoded text field exceeds the permitted length");
    return {};
  }
  if (!need(declared)) {
    return {};
  }
  const char* begin = reinterpret_cast<const char*>(data_.data() + pos_);
  std::string out(begin, static_cast<std::size_t>(declared));
  pos_ += declared;
  return out;
}

std::vector<std::uint8_t> Decoder::blob(std::size_t max_bytes) {
  if (!status_.ok()) {
    return {};
  }
  const std::uint32_t declared = u32();
  if (!status_.ok()) {
    return {};
  }
  const std::size_t limit = std::min(max_bytes, kMaxBlobBytes);
  if (declared > limit) {
    fail(ErrorCode::BoundsExceeded, "decoded blob exceeds the permitted length");
    return {};
  }
  if (!need(declared)) {
    return {};
  }
  std::vector<std::uint8_t> out(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                data_.begin() + static_cast<std::ptrdiff_t>(pos_ + declared));
  pos_ += declared;
  return out;
}

std::size_t Decoder::count(std::size_t max_items) noexcept {
  const std::uint32_t declared = u32();
  if (!status_.ok()) {
    return 0;
  }
  const std::size_t limit = std::min(max_items, kMaxCollectionItems);
  if (declared > limit) {
    fail(ErrorCode::BoundsExceeded, "decoded collection count exceeds the permitted item count");
    return 0;
  }
  if (static_cast<std::size_t>(declared) > remaining()) {
    fail(ErrorCode::StoreTruncated, "decoded collection count exceeds the remaining input");
    return 0;
  }
  return declared;
}

bool Decoder::count_equals(std::size_t expected, std::string_view what) noexcept {
  const std::size_t actual = count(expected);
  if (!status_.ok()) {
    return false;
  }
  if (actual != expected) {
    fail(ErrorCode::InvalidArgument, std::string("collection arity mismatch for ") + std::string(what));
    return false;
  }
  return true;
}

Result<BasisPoints> Decoder::basis_points() noexcept {
  const std::uint32_t raw = u32();
  if (!status_.ok()) {
    return status_;
  }
  if (raw > static_cast<std::uint32_t>(BasisPoints::scale())) {
    fail(ErrorCode::OutOfRange, "decoded basis points exceed 10000");
    return status_;
  }
  return BasisPoints(static_cast<std::int32_t>(raw));
}

Digest Decoder::digest() noexcept {
  Digest out;
  if (!need(Digest::kBytes)) {
    return out;
  }
  std::copy(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
            data_.begin() + static_cast<std::ptrdiff_t>(pos_ + Digest::kBytes),
            out.mutable_bytes().begin());
  pos_ += Digest::kBytes;
  return out;
}

Result<void> Decoder::require_end(std::string_view what) const {
  if (!status_.ok()) {
    return status_;
  }
  if (!at_end()) {
    return Status::error(ErrorCode::StoreTrailingBytes, std::string("trailing bytes after ") +
                                                            std::string(what),
                         "remaining", static_cast<std::uint64_t>(remaining()));
  }
  return Result<void>();
}

bool Decoder::reserved_zero(std::string_view what) noexcept {
  const std::uint32_t value = u32();
  if (!status_.ok()) {
    return false;
  }
  if (value != 0) {
    fail(ErrorCode::StoreReservedFieldSet,
         std::string("reserved field must be zero: ") + std::string(what));
    return false;
  }
  return true;
}

bool Decoder::reserved_u32(std::uint32_t expected, std::string_view what) noexcept {
  const std::uint32_t value = u32();
  if (!status_.ok()) {
    return false;
  }
  if (value != expected) {
    fail(ErrorCode::StoreReservedFieldSet,
         std::string("reserved field has an unexpected value: ") + std::string(what));
    return false;
  }
  return true;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (std::uint8_t byte : bytes) {
    out.push_back(kDigits[(byte >> 4) & 0x0FU]);
    out.push_back(kDigits[byte & 0x0FU]);
  }
  return out;
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view text) {
  if (text.size() % 2 != 0) {
    return Status::error(ErrorCode::InvalidText, "hex text must have an even length");
  }
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    std::uint32_t value = 0;
    for (std::size_t half = 0; half < 2; ++half) {
      const char c = text[index + half];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a') + 10U;
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A') + 10U;
      } else {
        return Status::error(ErrorCode::InvalidText, "hex text contains a non-hex character", "index",
                             static_cast<std::uint64_t>(index + half));
      }
      value = (value << 4) | digit;
    }
    out.push_back(static_cast<std::uint8_t>(value));
  }
  return out;
}

}  // namespace cooling_failover
