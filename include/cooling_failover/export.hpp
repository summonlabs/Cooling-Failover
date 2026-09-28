// Cooling Failover - export/visibility macros.
//
// Part of the Data Center Control Plane (DCCP).
#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(COOLING_FAILOVER_SHARED)
#    if defined(COOLING_FAILOVER_BUILDING_LIBRARY)
#      define CF_API __declspec(dllexport)
#    else
#      define CF_API __declspec(dllimport)
#    endif
#  else
#    define CF_API
#  endif
#  define CF_HIDDEN
#else
#  if defined(COOLING_FAILOVER_SHARED) && defined(COOLING_FAILOVER_BUILDING_LIBRARY)
#    define CF_API __attribute__((visibility("default")))
#    define CF_HIDDEN __attribute__((visibility("hidden")))
#  else
#    define CF_API
#    define CF_HIDDEN
#  endif
#endif
