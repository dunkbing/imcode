# auto-update through imshell's updater (-DIMCODE_UPDATER=ON; packaging turns it on). the feed
# and key are cache variables, empty by default: an empty feed leaves the updater off
#   macos: sparkle, feed and key in Info.plist (scripts/package-macos.sh writes them)
#   windows: winsparkle with the appcast and key below; the installer stages the update
#   linux: the AppImage swaps itself, from the json manifest
option(IMCODE_UPDATER "Auto-update" OFF)
set(IMCODE_APPCAST_URL "" CACHE STRING "windows: the sparkle appcast")
set(IMCODE_UPDATE_KEY "" CACHE STRING "windows: the appcast's ed25519 public key (base64)")
set(IMCODE_MANIFEST_URL "" CACHE STRING "linux: the json manifest")
if(NOT IMCODE_UPDATER)
    return()
endif()
set(IMSHELL_UPDATER ON CACHE BOOL "" FORCE)
include(FetchContent)
if(APPLE)
    FetchContent_Declare(sparkle URL https://github.com/sparkle-project/Sparkle/releases/download/2.9.1/Sparkle-2.9.1.tar.xz
        URL_HASH SHA256=c0dde519fd2a43ddfc6a1eb76aec284d7d888fe281414f9177de3164d98ba4c7)
    FetchContent_MakeAvailable(sparkle)
    set(IMSHELL_SPARKLE_FRAMEWORK ${sparkle_SOURCE_DIR}/Sparkle.framework CACHE PATH "" FORCE)
elseif(WIN32)
    FetchContent_Declare(winsparkle URL https://github.com/vslavik/winsparkle/releases/download/v0.9.2/WinSparkle-0.9.2.zip
        URL_HASH SHA256=2a5facf5a22056edf7afc4d0a4bf05620b7aa7cc1619c3078b281e374cdb9996)
    FetchContent_MakeAvailable(winsparkle)
    add_library(WinSparkle SHARED IMPORTED)
    set_target_properties(WinSparkle PROPERTIES
        IMPORTED_IMPLIB ${winsparkle_SOURCE_DIR}/x64/Release/WinSparkle.lib
        IMPORTED_LOCATION ${winsparkle_SOURCE_DIR}/x64/Release/WinSparkle.dll
        INTERFACE_INCLUDE_DIRECTORIES ${winsparkle_SOURCE_DIR}/include)
endif()
