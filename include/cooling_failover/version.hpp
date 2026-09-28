// Cooling Failover - version identity.
#pragma once

#include "cooling_failover/export.hpp"

#include <cstdint>
#include <string_view>

#define COOLING_FAILOVER_VERSION_MAJOR 1
#define COOLING_FAILOVER_VERSION_MINOR 0
#define COOLING_FAILOVER_VERSION_PATCH 0

namespace cooling_failover {

inline constexpr std::uint32_t kVersionMajor = COOLING_FAILOVER_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = COOLING_FAILOVER_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = COOLING_FAILOVER_VERSION_PATCH;

/// On-disk / on-wire format version owned by this repository. Independent of the
/// product version: it changes only when a persisted encoding changes shape.
inline constexpr std::uint32_t kStoreFormatVersion = 1;

/// Human readable product version, e.g. "1.0.0".
[[nodiscard]] CF_API std::string_view version_string() noexcept;

/// Build identity string including compiler and configuration, e.g.
/// "1.0.0+msvc-19.44.35209/release".
[[nodiscard]] CF_API std::string_view build_identity() noexcept;

}  // namespace cooling_failover
