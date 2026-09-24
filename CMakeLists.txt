cmake_minimum_required(VERSION 3.20)
project(seerr_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(SeerrOptions)
include(SeerrDependencies)
include(SeerrImGui)

# ---------- app ----------
set(SEERR_SOURCES
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
    src/i18n.cpp
    src/gl_load.cpp
)

add_executable(seerr ${SEERR_SOURCES})

target_include_directories(seerr PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/vendor
)

# Runtime asset discovery prefers paths next to the binary / /usr/share/seerr.
# APP_* are compile-time fallbacks for in-tree developer builds only.
target_compile_definitions(seerr PRIVATE
    APP_ASSET_DIR="${CMAKE_SOURCE_DIR}/vendor"
    APP_LOCALE_DIR="${CMAKE_SOURCE_DIR}/locales"
    GLFW_INCLUDE_NONE
)

target_link_libraries(seerr PRIVATE imgui ${SEERR_TORRENT_TARGET} Boost::headers)

if(WIN32)
    target_link_libraries(seerr PRIVATE
        winhttp shell32 ole32 oleaut32 gdi32 user32 winmm version uuid opengl32 shlwapi advapi32 psapi
        ws2_32 iphlpapi dwmapi
        z
    )
    # GUI subsystem — no black console window.
    set_target_properties(seerr PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(seerr PRIVATE -mwindows)
    if(SEERR_BUNDLED_DEPS)
        target_link_options(seerr PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(seerr PRIVATE CURL::libcurl Threads::Threads dl ZLIB::ZLIB)
endif()

# Lay out assets next to the binary so `./build/seerr` works without install.
if(SEERR_COPY_ASSETS_TO_BUILD)
    set(_seerr_asset_cmds
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:seerr>/fonts"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:seerr>/icons"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:seerr>/locales"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/vendor/logo_full.png"
            "${CMAKE_SOURCE_DIR}/vendor/icon.png"
            "${CMAKE_SOURCE_DIR}/vendor/poster_missing.png"
            "$<TARGET_FILE_DIR:seerr>/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/vendor/fonts" "$<TARGET_FILE_DIR:seerr>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/vendor/icons" "$<TARGET_FILE_DIR:seerr>/icons"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/locales" "$<TARGET_FILE_DIR:seerr>/locales"
    )
    add_custom_command(TARGET seerr POST_BUILD
        ${_seerr_asset_cmds}
        COMMENT "Staging runtime assets next to seerr"
        VERBATIM
    )
endif()

include(SeerrPackaging)

message(STATUS "Seerr ${SEERR_VERSION} — build type: ${CMAKE_BUILD_TYPE}")
