cmake_minimum_required(VERSION 3.20)
project(seerr_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Prefer system packages in CI; bundle on local Windows by default.
if(NOT DEFINED SEERR_BUNDLED_DEPS)
    if(WIN32)
        set(SEERR_BUNDLED_DEPS ON)
    else()
        set(SEERR_BUNDLED_DEPS OFF)
    endif()
endif()
option(SEERR_BUNDLED_DEPS "Fetch Boost + libtorrent via FetchContent" ${SEERR_BUNDLED_DEPS})

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# ---------- libtorrent + Boost ----------
if(SEERR_BUNDLED_DEPS)
    FetchContent_Declare(
        boost_src
        URL https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(boost_src)
    if(NOT boost_src_POPULATED)
        FetchContent_Populate(boost_src)
    endif()

    set(BOOST_ROOT "${boost_src_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(Boost_INCLUDE_DIR "${boost_src_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(Boost_NO_SYSTEM_PATHS ON)
    set(Boost_NO_BOOST_CMAKE ON)
    find_package(Boost 1.70 REQUIRED)
    if(NOT TARGET Boost::headers)
        add_library(seerr_boost_headers INTERFACE)
        add_library(Boost::headers ALIAS seerr_boost_headers)
        target_include_directories(seerr_boost_headers INTERFACE ${Boost_INCLUDE_DIRS})
    endif()

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(deprecated-functions OFF CACHE BOOL "" FORCE)
    set(encryption OFF CACHE BOOL "" FORCE)
    set(build_tests OFF CACHE BOOL "" FORCE)
    set(build_examples OFF CACHE BOOL "" FORCE)
    set(build_tools OFF CACHE BOOL "" FORCE)
    set(python-bindings OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        libtorrent
        GIT_REPOSITORY https://github.com/arvidn/libtorrent.git
        GIT_TAG        v2.0.11
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(libtorrent)
    set(SEERR_TORRENT_TARGET torrent-rasterbar)
else()
    find_package(Boost 1.70 REQUIRED)
    if(NOT TARGET Boost::headers)
        add_library(seerr_boost_headers INTERFACE)
        add_library(Boost::headers ALIAS seerr_boost_headers)
        target_include_directories(seerr_boost_headers INTERFACE ${Boost_INCLUDE_DIRS})
    endif()

    find_package(PkgConfig QUIET)
    set(SEERR_TORRENT_TARGET "")
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBTORRENT libtorrent-rasterbar)
        if(LIBTORRENT_FOUND)
            # MSYS2 pkg-config emits /mingw64/... which CMake on Windows cannot use as-is.
            # Only rewrite paths/flags that START with /mingw64 — do not touch already-absolute
            # Windows paths that happen to contain ".../mingw64/..." mid-string.
            set(_msys_prefix "")
            if(DEFINED ENV{MSYSTEM_PREFIX} AND NOT "$ENV{MSYSTEM_PREFIX}" STREQUAL "")
                set(_msys_prefix "$ENV{MSYSTEM_PREFIX}")
            elseif(EXISTS "C:/msys64/mingw64")
                set(_msys_prefix "C:/msys64/mingw64")
            endif()
            if(_msys_prefix)
                foreach(_var LIBTORRENT_INCLUDE_DIRS LIBTORRENT_LIBRARY_DIRS)
                    set(_fixed "")
                    foreach(_p IN LISTS ${_var})
                        if(_p MATCHES "^/mingw64")
                            string(REGEX REPLACE "^/mingw64" "${_msys_prefix}" _p "${_p}")
                        endif()
                        list(APPEND _fixed "${_p}")
                    endforeach()
                    set(${_var} "${_fixed}")
                endforeach()
                set(_fixed "")
                foreach(_p IN LISTS LIBTORRENT_LDFLAGS)
                    if(_p MATCHES "^-L/mingw64")
                        string(REGEX REPLACE "^-L/mingw64" "-L${_msys_prefix}" _p "${_p}")
                    elseif(_p MATCHES "^/mingw64")
                        string(REGEX REPLACE "^/mingw64" "${_msys_prefix}" _p "${_p}")
                    endif()
                    list(APPEND _fixed "${_p}")
                endforeach()
                set(LIBTORRENT_LDFLAGS "${_fixed}")
            endif()
            # Deduplicate library dirs (pkg-config often lists both Windows and /mingw64 forms)
            if(LIBTORRENT_LIBRARY_DIRS)
                list(REMOVE_DUPLICATES LIBTORRENT_LIBRARY_DIRS)
            endif()
            if(LIBTORRENT_INCLUDE_DIRS)
                list(REMOVE_DUPLICATES LIBTORRENT_INCLUDE_DIRS)
            endif()
            # Drop "-isystem <mingw prefix>/include" from pkg-config Cflags: putting the C
            # sysroot dir BEFORE the C++ stdlib dirs breaks #include_next <stdlib.h>
            # (fatal error: stdlib.h: No such file or directory). The compiler already
            # searches the sysroot as a fallback directory anyway.
            set(_lt_cflags "")
            set(_lt_drop_next FALSE)
            foreach(_lt_flag IN LISTS LIBTORRENT_CFLAGS_OTHER)
                if(_lt_drop_next)
                    set(_lt_drop_next FALSE)
                    continue()
                endif()
                if(_lt_flag STREQUAL "-isystem")
                    set(_lt_drop_next TRUE)
                    continue()
                endif()
                string(FIND "${_lt_flag}" "-isystem" _lt_pos)
                if(_lt_pos EQUAL 0)
                    continue() # e.g. -isystem/path forms
                endif()
                list(APPEND _lt_cflags "${_lt_flag}")
            endforeach()
            set(LIBTORRENT_CFLAGS_OTHER "${_lt_cflags}")
            add_library(seerr_libtorrent INTERFACE)
            target_include_directories(seerr_libtorrent INTERFACE ${LIBTORRENT_INCLUDE_DIRS})
            target_link_directories(seerr_libtorrent INTERFACE ${LIBTORRENT_LIBRARY_DIRS})
            target_link_libraries(seerr_libtorrent INTERFACE ${LIBTORRENT_LIBRARIES})
            target_compile_options(seerr_libtorrent INTERFACE ${LIBTORRENT_CFLAGS_OTHER})
            set(SEERR_TORRENT_TARGET seerr_libtorrent)
        endif()
    endif()
    if(SEERR_TORRENT_TARGET STREQUAL "")
        find_package(LibtorrentRasterbar REQUIRED)
        if(TARGET LibtorrentRasterbar::torrent-rasterbar)
            set(SEERR_TORRENT_TARGET LibtorrentRasterbar::torrent-rasterbar)
        elseif(TARGET torrent-rasterbar)
            set(SEERR_TORRENT_TARGET torrent-rasterbar)
        else()
            message(FATAL_ERROR "libtorrent-rasterbar not found (install libtorrent-rasterbar-dev)")
        endif()
    endif()
endif()

# ---------- vendor: imgui ----------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(GLFW_DIR ${CMAKE_SOURCE_DIR}/vendor/glfw)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC GLFW_INCLUDE_NONE)

if (WIN32)
    target_include_directories(imgui PUBLIC ${GLFW_DIR}/include)
    target_link_libraries(imgui PUBLIC ${GLFW_DIR}/lib-mingw-w64/libglfw3.a opengl32 gdi32)
else()
    # Do not link OpenGL::GL — GLVND stubs segfault if called without a loader.
    # GLFW pulls in GLX/EGL; ImGui + seerrLoadGL resolve entry points at runtime.
    find_package(glfw3 REQUIRED)
    target_link_libraries(imgui PUBLIC glfw ${CMAKE_DL_LIBS})
endif()

# ---------- app ----------
add_executable(seerr
    src/main.cpp
    src/util.cpp
    src/http.cpp
    src/imgcache.cpp
    src/imgswarm.cpp
    src/tmdb.cpp
    src/widgets.cpp
    src/pages.cpp
    src/ytplayer.cpp
    src/svgicons.cpp
    src/stack.cpp
    src/core.cpp
    src/torrent.cpp
    src/localdb.cpp
    src/platform.cpp
    src/library.cpp
    src/player.cpp
    src/subs.cpp
    src/i18n.cpp
    src/gl_load.cpp
)
target_include_directories(seerr PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/vendor
)
target_compile_definitions(seerr PRIVATE
    APP_ASSET_DIR="${CMAKE_SOURCE_DIR}/vendor"
    APP_LOCALE_DIR="${CMAKE_SOURCE_DIR}/locales"
    GLFW_INCLUDE_NONE
)
target_link_libraries(seerr PRIVATE imgui ${SEERR_TORRENT_TARGET} Boost::headers)

if (WIN32)
    target_link_libraries(seerr PRIVATE
        winhttp shell32 ole32 oleaut32 gdi32 user32 winmm version uuid opengl32 shlwapi advapi32 psapi
        ws2_32 iphlpapi dwmapi
        z # gzip for NapiProjekt subtitles (subs.cpp); zlib.h is on the compiler default path,
          # do NOT add it via include dirs — -isystem before the C++ sysroot breaks include_next
    )
    if(SEERR_BUNDLED_DEPS)
        target_link_options(seerr PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(seerr PRIVATE CURL::libcurl Threads::Threads dl z)
endif()

# ---------- install / .deb packaging ----------
include(GNUInstallDirs)

set(SEERR_VERSION "0.1.0" CACHE STRING "Package version")
set(PROJECT_VERSION "${SEERR_VERSION}")

install(TARGETS seerr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES
    ${CMAKE_SOURCE_DIR}/vendor/logo_full.png
    ${CMAKE_SOURCE_DIR}/vendor/icon.png
    ${CMAKE_SOURCE_DIR}/vendor/poster_missing.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/seerr
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/vendor/fonts/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/seerr/fonts
    FILES_MATCHING PATTERN "*.ttf"
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/vendor/icons/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/seerr/icons
    FILES_MATCHING PATTERN "*.svg"
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/locales/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/seerr/locales
    FILES_MATCHING PATTERN "*.json"
)

install(FILES ${CMAKE_SOURCE_DIR}/packaging/seerr.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

install(FILES ${CMAKE_SOURCE_DIR}/vendor/icon.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps
    RENAME seerr.png
)

set(CPACK_PACKAGE_NAME "seerr")
set(CPACK_PACKAGE_VENDOR "tezeusz")
set(CPACK_PACKAGE_CONTACT "tezeusz <noreply@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Seerr-style media discovery and download desktop app")
set(CPACK_PACKAGE_DESCRIPTION "Desktop Jellyseerr/Seerr-style media discovery app (ImGui + GLFW) with a built-in torrent download stack.")
set(CPACK_PACKAGE_VERSION "${SEERR_VERSION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/hdmain/tezeusz")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS OFF)
# Fallback Depends if shlibdeps is unavailable in the packaging environment
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libglfw3 | libglfw3-wayland, libcurl4 | libcurl3t64, zlib1g, libstdc++6")
include(CPack)
