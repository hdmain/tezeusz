# Seerr CMake helpers — keep the root CMakeLists thin and predictable.

# Prefer system packages in CI; bundle on local Windows by default.
if(NOT DEFINED SEERR_BUNDLED_DEPS)
    if(WIN32)
        set(SEERR_BUNDLED_DEPS ON)
    else()
        set(SEERR_BUNDLED_DEPS OFF)
    endif()
endif()
option(SEERR_BUNDLED_DEPS "Fetch Boost + libtorrent via FetchContent" ${SEERR_BUNDLED_DEPS})
option(SEERR_COPY_ASSETS_TO_BUILD
    "Copy/symlink fonts, icons, locales next to the built binary (local runs)"
    ON)

set(SEERR_VERSION "0.1.0" CACHE STRING "Package version")
set(PROJECT_VERSION "${SEERR_VERSION}")

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
