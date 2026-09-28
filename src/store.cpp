#include "cooling_failover/store.hpp"

#include "cooling_failover/version.hpp"
#include "store_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

namespace cooling_failover {
namespace {

constexpr std::uint32_t kRecordMagic = 0x4B524643U;  // "CFRK"

[[nodiscard]] std::string domain_directory_name(FailoverDomainId domain) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string name;
  name.reserve(16);
  for (int shift = 60; shift >= 0; shift -= 4) {
    name.push_back(kDigits[(domain.value() >> shift) & 0xFU]);
  }
  return name;
}

[[nodiscard]] bool is_reserved_component(const std::string& upper) {
  static const char* const kReserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2",
                                          "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                          "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                          "LPT6", "LPT7", "LPT8", "LPT9"};
  for (const char* name : kReserved) {
    if (upper == name) {
      return true;
    }
  }
  return false;
}

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

#ifdef _WIN32
[[nodiscard]] bool wide_equal_case_insensitive(const std::wstring& lhs, const std::wstring& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (std::towlower(lhs[index]) != std::towlower(rhs[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::wstring canonical_of_handle(void* handle) {
  std::vector<wchar_t> buffer(32768, L'\0');
  const DWORD written = ::GetFinalPathNameByHandleW(static_cast<HANDLE>(handle), buffer.data(),
                                                    static_cast<DWORD>(buffer.size()),
                                                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (written == 0 || written >= buffer.size()) {
    return {};
  }
  std::wstring path(buffer.data(), written);
  const std::wstring prefix = L"\\\\?\\";
  if (path.rfind(prefix, 0) == 0) {
    path = path.substr(prefix.size());
  }
  return path;
}
#endif

}  // namespace

bool is_known_record_type(std::uint16_t raw) noexcept {
  switch (static_cast<RecordType>(raw)) {
    case RecordType::DomainRegistered:
    case RecordType::EpochEstablished:
    case RecordType::TopologyImported:
    case RecordType::CapacityRecorded:
    case RecordType::HealthRecorded:
    case RecordType::AuthorityRecorded:
    case RecordType::PolicyInstalled:
    case RecordType::ObligationsInstalled:
    case RecordType::PlanCreated:
    case RecordType::PlanStateChanged:
    case RecordType::CommandRecorded:
    case RecordType::ObservationRecorded:
    case RecordType::AttemptRecorded:
    case RecordType::TransitionRecorded:
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Path preparation
// ---------------------------------------------------------------------------

Result<std::filesystem::path> prepare_store_root(const std::filesystem::path& root) {
  const std::string raw = root.string();
  if (raw.empty()) {
    return Status::error(ErrorCode::InvalidPath, "store root is empty");
  }
  if (raw.find('\0') != std::string::npos) {
    return Status::error(ErrorCode::InvalidPath, "store root contains an embedded NUL");
  }
  if (raw.size() > kMaxStorePathChars) {
    return Status::error(ErrorCode::PathTooLong, "store root exceeds the path length bound",
                         "length", static_cast<std::uint64_t>(raw.size()));
  }
  for (char c : raw) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte < 0x20U || byte == 0x7FU) {
      return Status::error(ErrorCode::InvalidPath, "store root contains a control character",
                           "byte", static_cast<std::uint64_t>(byte));
    }
  }
  if (raw.rfind("\\\\?\\", 0) == 0 || raw.rfind("\\\\.\\", 0) == 0) {
    return Status::error(ErrorCode::PathAmbiguous, "store root uses a device path prefix");
  }
  if (raw.find("::") != std::string::npos) {
    return Status::error(ErrorCode::PathAmbiguous, "store root uses an alternate data stream");
  }
  if (raw.size() > 1 && raw[1] == ':') {
    if (raw.find(':', 2) != std::string::npos) {
      return Status::error(ErrorCode::PathAmbiguous, "store root contains a drive-relative prefix");
    }
  } else if (raw.find(':') != std::string::npos) {
    return Status::error(ErrorCode::PathAmbiguous, "store root contains a drive-relative prefix");
  }

  std::vector<std::string> components;
  std::string current;
  for (std::size_t index = 0; index < raw.size(); ++index) {
    const char c = raw[index];
    if (c == '\\' || c == '/') {
      if (!current.empty()) {
        components.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    components.push_back(current);
  }

  for (const std::string& component : components) {
    if (component == "." || component == "..") {
      return Status::error(ErrorCode::InvalidPath,
                           "store root contains a relative traversal component", "component",
                           component);
    }
    if (component.size() > kMaxPathComponentChars) {
      return Status::error(ErrorCode::PathTooLong, "store root component is too long", "component",
                           component);
    }
    if (component.back() == '.' || component.back() == ' ') {
      return Status::error(ErrorCode::InvalidPath, "store root component ends with a dot or space",
                           "component", component);
    }
    for (char c : component) {
      if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
        return Status::error(ErrorCode::InvalidPath,
                             "store root component contains a reserved character", "component",
                             component);
      }
    }
    std::string upper = component;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const std::size_t dot = upper.find('.');
    if (dot != std::string::npos) {
      upper = upper.substr(0, dot);
    }
    if (is_reserved_component(upper)) {
      return Status::error(ErrorCode::InvalidPath, "store root component is a reserved device name",
                           "component", component);
    }
  }

  std::error_code code;
  const std::filesystem::path requested = std::filesystem::absolute(root, code);
  if (code) {
    return Status::error(ErrorCode::InvalidPath, "store root could not be made absolute", "error",
                         code.message());
  }

  std::filesystem::path walked;
  for (const std::filesystem::path& part : requested) {
    walked /= part;
    std::error_code exists_code;
    if (std::filesystem::exists(walked, exists_code) && !exists_code) {
      if (detail::is_reparse_point_path(walked)) {
        return Status::error(ErrorCode::PathReparsePoint, "store root traverses a reparse point",
                             "component", walked.string());
      }
    } else {
      break;
    }
  }

  std::filesystem::create_directories(requested, code);
  if (code) {
    return Status::error(ErrorCode::StoreIoError, "store root could not be created", "error",
                         code.message());
  }
  if (!std::filesystem::is_directory(requested, code) || code) {
    return Status::error(ErrorCode::InvalidPath, "store root is not a directory", "path",
                         requested.string());
  }

  const std::filesystem::path canonical = std::filesystem::weakly_canonical(requested, code);
  if (code || canonical.empty()) {
    return Status::error(ErrorCode::InvalidPath, "store root could not be canonicalized");
  }
  return canonical;
}

std::filesystem::path domain_store_directory(const std::filesystem::path& prepared_root,
                                             FailoverDomainId domain) {
  return prepared_root / domain_directory_name(domain);
}

// ---------------------------------------------------------------------------
// RecordFrame
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> RecordFrame::encode(RecordType type, CommitSequence sequence,
                                              std::span<const std::uint8_t> payload) {
  const std::size_t body_bytes = payload.size() + 2;
  std::vector<std::uint8_t> frame(kHeaderBytes + body_bytes, 0);
  std::vector<std::uint8_t> body(body_bytes, 0);
  const std::uint16_t raw_type = static_cast<std::uint16_t>(type);
  body[0] = static_cast<std::uint8_t>(raw_type & 0xFFU);
  body[1] = static_cast<std::uint8_t>((raw_type >> 8) & 0xFFU);
  std::copy(payload.begin(), payload.end(), body.begin() + 2);

  put_u32(frame, 0, kRecordMagic);
  put_u32(frame, 4, static_cast<std::uint32_t>(body_bytes));
  put_u64(frame, 8, sequence.value());
  put_u32(frame, 16, crc32(body));
  put_u32(frame, 20, crc32(std::span<const std::uint8_t>(frame.data() + 4, 16)));
  std::copy(body.begin(), body.end(), frame.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes));
  return frame;
}

Result<RecordFrame> RecordFrame::decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kHeaderBytes) {
    return Status::error(ErrorCode::StoreTruncated, "record frame is shorter than its header");
  }
  if (get_u32(bytes, 0) != kRecordMagic) {
    return Status::error(ErrorCode::StoreCorrupt, "record magic mismatch");
  }
  if (crc32(bytes.subspan(4, 16)) != get_u32(bytes, 20)) {
    return Status::error(ErrorCode::StoreCorrupt, "record header integrity check failed");
  }
  const std::uint32_t body_len = get_u32(bytes, 4);
  if (body_len < 2) {
    return Status::error(ErrorCode::StoreCorrupt, "record payload is too short to hold its type");
  }
  if (body_len > kMaxBlobBytes) {
    return Status::error(ErrorCode::StoreCorrupt, "record payload declares an impossible length",
                         "declared", static_cast<std::uint64_t>(body_len));
  }
  if (bytes.size() < kHeaderBytes + body_len) {
    return Status::error(ErrorCode::StoreTruncated, "record payload is incomplete", "declared",
                         static_cast<std::uint64_t>(body_len), "available",
                         static_cast<std::uint64_t>(bytes.size() - kHeaderBytes));
  }
  const std::span<const std::uint8_t> body = bytes.subspan(kHeaderBytes, body_len);
  if (crc32(body) != get_u32(bytes, 16)) {
    return Status::error(ErrorCode::StoreCorrupt, "record payload integrity check failed");
  }
  const std::uint16_t raw_type =
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(body[0]) |
                                 static_cast<std::uint16_t>(static_cast<std::uint16_t>(body[1]) << 8));
  if (!is_known_record_type(raw_type)) {
    return Status::error(ErrorCode::InvalidEnumValue, "record type is not recognised", "type",
                         static_cast<std::uint64_t>(raw_type));
  }
  RecordFrame frame;
  frame.type = static_cast<RecordType>(raw_type);
  frame.sequence = CommitSequence::from_value(get_u64(bytes, 8));
  frame.payload.assign(body.begin() + 2, body.end());
  frame.frame_bytes = kHeaderBytes + body_len;
  return frame;
}

// ---------------------------------------------------------------------------
// WriterLock
// ---------------------------------------------------------------------------

WriterLock::WriterLock(WriterLock&& other) noexcept
    : handle_(other.handle_),
      process_id_(other.process_id_),
      incarnation_(other.incarnation_),
      canonical_path_(std::move(other.canonical_path_)) {
  other.handle_ = nullptr;
  other.process_id_ = 0;
}

WriterLock& WriterLock::operator=(WriterLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    process_id_ = other.process_id_;
    incarnation_ = other.incarnation_;
    canonical_path_ = std::move(other.canonical_path_);
    other.handle_ = nullptr;
    other.process_id_ = 0;
  }
  return *this;
}

WriterLock::~WriterLock() { release(); }

void WriterLock::release() noexcept {
  if (handle_ == nullptr) {
    return;
  }
#ifdef _WIN32
  ::CloseHandle(static_cast<HANDLE>(handle_));
#else
  ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)));
#endif
  handle_ = nullptr;
  canonical_path_.clear();
}

Result<WriterLock> WriterLock::acquire(const std::filesystem::path& lock_path) {
  WriterLock lock;
#ifdef _WIN32
  HANDLE handle = ::CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
      return Status::error(ErrorCode::StoreLocked, "another writer holds this domain store", "path",
                           lock_path.string());
    }
    return Status::error(ErrorCode::StoreIoError, "writer lock could not be opened", "path",
                         lock_path.string(), "code", static_cast<std::uint64_t>(error));
  }
  const std::wstring final_path = canonical_of_handle(handle);
  std::error_code code;
  const std::filesystem::path expected = std::filesystem::weakly_canonical(lock_path, code);
  if (code || final_path.empty() ||
      !wide_equal_case_insensitive(final_path, expected.wstring())) {
    ::CloseHandle(handle);
    return Status::error(ErrorCode::PathAmbiguous,
                         "writer lock canonical path does not match the requested path",
                         "requested", lock_path.string());
  }
  lock.handle_ = handle;
  lock.process_id_ = static_cast<std::uint32_t>(::GetCurrentProcessId());
  lock.canonical_path_ = expected.string();
#else
  const int descriptor = ::open(lock_path.string().c_str(), O_RDWR | O_CREAT, 0600);
  if (descriptor < 0) {
    return Status::error(ErrorCode::StoreIoError, "writer lock could not be opened", "path",
                         lock_path.string());
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    ::close(descriptor);
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return Status::error(ErrorCode::StoreLocked, "another writer holds this domain store", "path",
                           lock_path.string());
    }
    return Status::error(ErrorCode::StoreIoError, "writer lock could not be taken", "path",
                         lock_path.string());
  }
  lock.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor));
  lock.process_id_ = static_cast<std::uint32_t>(::getpid());
  std::error_code code;
  const std::filesystem::path expected = std::filesystem::weakly_canonical(lock_path, code);
  lock.canonical_path_ = code ? lock_path.string() : expected.string();
#endif

  Encoder encoder;
  encoder.u64(lock.process_id_);
  encoder.text(lock.canonical_path_);
  encoder.u64(detail::process_start_marker());
  lock.incarnation_ = encoder.finish_digest();
  return lock;
}

}  // namespace cooling_failover
