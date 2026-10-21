# Modified 2026-10-20 for the icon-independent publication source candidate.
# The approved local icon is preserved in the private development baseline.
# No replacement artwork or operating-system icon asset is distributed here.
set(RWE_MACOS_ICON_FORMAT "NONE" CACHE STRING "Publication icon policy: NONE")
set_property(CACHE RWE_MACOS_ICON_FORMAT PROPERTY STRINGS NONE)
if(NOT RWE_MACOS_ICON_FORMAT STREQUAL "NONE")
    message(FATAL_ERROR "This source distribution contains no cleared app icon; select NONE in a fresh build directory")
endif()
if(DEFINED RWE_MACOS_ICON_MODERN_CONFIGURED)
    message(FATAL_ERROR "A previous icon-enabled configuration exists; use a fresh build directory")
endif()
set(RWE_MACOS_ICON_POLICY "NONE" CACHE INTERNAL "No app icon resources are bundled")
message(STATUS "TA Transplant icon: omitted by publication policy (NONE)")
