cmake_minimum_required(VERSION 3.20)
project(tezeusz_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(TezeuszOptions)
include(TezeuszDependencies)
include(TezeuszImGui)

# ---------- app ----------
set(TEZEUSZ_SOURCES
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
    src/tray.cpp
    src/library.cpp
    src/player.cpp
    src/subs.cpp
    src/lector.cpp
    src/i18n.cpp
    src/gl_load.cpp
    src/updater.cpp
    src/modalblur.cpp
)

add_executable(tezeusz ${TEZEUSZ_SOURCES})

# Embed application icon in the Windows PE (taskbar / Explorer / shortcuts).
if(WIN32)
    enable_language(RC)
    target_sources(tezeusz PRIVATE "${CMAKE_SOURCE_DIR}/packaging/tezeusz.rc")
endif()

target_include_directories(tezeusz PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/vendor
)

# Runtime asset discovery prefers paths next to the binary / /usr/share/tezeusz.
# APP_* are compile-time fallbacks for in-tree developer builds only.
target_compile_definitions(tezeusz PRIVATE
    APP_ASSET_DIR="${CMAKE_SOURCE_DIR}/vendor"
    APP_LOCALE_DIR="${CMAKE_SOURCE_DIR}/locales"
    GLFW_INCLUDE_NONE
    TEZEUSZ_VERSION="${TEZEUSZ_VERSION}"
    TEZEUSZ_UPDATE_REPO="hdmain/tezeusz"
    TEZEUSZ_UPDATE_CHANNEL="continuous"
)

target_link_libraries(tezeusz PRIVATE imgui ${TEZEUSZ_TORRENT_TARGET} Boost::headers)

if(WIN32)
    target_link_libraries(tezeusz PRIVATE
        winhttp shell32 ole32 oleaut32 gdi32 user32 winmm version uuid opengl32 shlwapi advapi32 psapi comdlg32
        ws2_32 iphlpapi dwmapi
        z
    )
    # GUI subsystem - no black console window.
    set_target_properties(tezeusz PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(tezeusz PRIVATE -mwindows)
    if(TEZEUSZ_BUNDLED_DEPS)
        target_link_options(tezeusz PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(tezeusz PRIVATE CURL::libcurl Threads::Threads dl ZLIB::ZLIB)
endif()

# Lay out assets next to the binary so `./build/tezeusz` works without install.
if(TEZEUSZ_COPY_ASSETS_TO_BUILD)
    set(_tezeusz_asset_cmds
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tezeusz>/fonts"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tezeusz>/icons"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tezeusz>/locales"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/logo_full.png"
            "${CMAKE_SOURCE_DIR}/vendor/icon.png"
            "${CMAKE_SOURCE_DIR}/vendor/poster_missing.png"
            "$<TARGET_FILE_DIR:tezeusz>/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/vendor/fonts" "$<TARGET_FILE_DIR:tezeusz>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/vendor/icons" "$<TARGET_FILE_DIR:tezeusz>/icons"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/locales" "$<TARGET_FILE_DIR:tezeusz>/locales"
    )
    add_custom_command(TARGET tezeusz POST_BUILD
        ${_tezeusz_asset_cmds}
        COMMENT "Staging runtime assets next to tezeusz"
        VERBATIM
    )
endif()

# Windows: copy MinGW runtime DLLs beside the exe so PowerShell/CMD can launch
# without MinGW on PATH. Best-effort only - must not fail CI (portable packager
# collects DLLs separately via bash).
if(WIN32)
    add_custom_command(TARGET tezeusz POST_BUILD
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/stage-mingw-dlls.ps1"
            -Exe "$<TARGET_FILE:tezeusz>"
        COMMENT "Staging MinGW DLLs next to tezeusz.exe (best-effort)"
        VERBATIM
    )
endif()

include(TezeuszPackaging)

message(STATUS "Tezeusz ${TEZEUSZ_VERSION} - build type: ${CMAKE_BUILD_TYPE}")
