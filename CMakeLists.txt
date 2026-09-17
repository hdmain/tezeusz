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
            # MSYS2 pkg-config emits /mingw64/... which CMake on Windows cannot use as-is
            if(DEFINED ENV{MSYSTEM_PREFIX})
                string(REPLACE "/mingw64" "$ENV{MSYSTEM_PREFIX}" LIBTORRENT_INCLUDE_DIRS "${LIBTORRENT_INCLUDE_DIRS}")
                string(REPLACE "/mingw64" "$ENV{MSYSTEM_PREFIX}" LIBTORRENT_LIBRARY_DIRS "${LIBTORRENT_LIBRARY_DIRS}")
                string(REPLACE "/mingw64" "$ENV{MSYSTEM_PREFIX}" LIBTORRENT_LDFLAGS "${LIBTORRENT_LDFLAGS}")
            endif()
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

if (WIN32)
    target_include_directories(imgui PUBLIC ${GLFW_DIR}/include)
    target_link_libraries(imgui PUBLIC ${GLFW_DIR}/lib-mingw-w64/libglfw3.a opengl32 gdi32)
else()
    find_package(OpenGL REQUIRED)
    find_package(glfw3 REQUIRED)
    target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
endif()

# ---------- app ----------
add_executable(seerr
    src/main.cpp
    src/util.cpp
    src/http.cpp
    src/imgcache.cpp
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
)
target_include_directories(seerr PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/vendor
)
target_compile_definitions(seerr PRIVATE
    APP_ASSET_DIR="${CMAKE_SOURCE_DIR}/vendor"
)
target_link_libraries(seerr PRIVATE imgui ${SEERR_TORRENT_TARGET} Boost::headers)

if (WIN32)
    target_link_libraries(seerr PRIVATE
        winhttp shell32 ole32 oleaut32 gdi32 user32 winmm version uuid opengl32 shlwapi advapi32 psapi
        ws2_32 iphlpapi
    )
    if(SEERR_BUNDLED_DEPS)
        target_link_options(seerr PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(seerr PRIVATE CURL::libcurl Threads::Threads dl)
endif()
