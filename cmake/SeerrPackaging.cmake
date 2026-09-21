# Install rules + CPack DEB.

include(GNUInstallDirs)

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
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libglfw3 | libglfw3-wayland, libcurl4 | libcurl3t64, zlib1g, libstdc++6, libvlc5 | vlc")
include(CPack)
