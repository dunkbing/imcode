cmake_minimum_required(VERSION 3.23)

# optimized unless asked otherwise: an unoptimized build (and vcpkg's debug libraries) makes git and
# the diff about 3x slower. before project(), which picks the vcpkg libraries
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_GENERATOR MATCHES "Xcode|Multi-Config|Visual Studio")
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
project(imcode VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(libssh2 CONFIG REQUIRED)
set(IMCODE_SSH $<IF:$<TARGET_EXISTS:libssh2::libssh2_shared>,libssh2::libssh2_shared,libssh2::libssh2_static>)
# git for projects on this machine (remote ones run git on their host)
find_package(libgit2 CONFIG REQUIRED)
list(APPEND IMCODE_SSH libgit2::libgit2package)
if(APPLE) # the keychain, for ssh passwords and passphrases
    list(APPEND IMCODE_SSH "-framework Security" "-framework CoreFoundation")
endif()

# libghostty-vt 1.0.0 (terminal emulation). apple: the official prebuilt static library.
# elsewhere there is none: its release source, whose own cmake runs `zig build` (zig >= 0.17)
# and defines the same ghostty-vt-static target. hashes from the release's SHA256SUMS
include(FetchContent)
set(GHOSTTY_VT_RELEASE https://release.files.ghostty.org/libghostty-vt/1.0.0)
if(APPLE)
    FetchContent_Declare(ghostty_vt URL ${GHOSTTY_VT_RELEASE}/ghostty-vt-1.0.0.xcframework.zip
        URL_HASH SHA256=2c32b7409b2b6c2ef6cfd606526de357762ad458bdc27134ad074e86506a8bc2)
    FetchContent_MakeAvailable(ghostty_vt)
    # shortcut: the macos slice; an ios build picks ios-arm64 or ios-arm64-simulator here
    add_library(ghostty-vt-static STATIC IMPORTED)
    set_target_properties(ghostty-vt-static PROPERTIES
        IMPORTED_LOCATION ${ghostty_vt_SOURCE_DIR}/macos-arm64_x86_64/libghostty-vt.a
        INTERFACE_INCLUDE_DIRECTORIES ${ghostty_vt_SOURCE_DIR}/macos-arm64_x86_64/Headers
        INTERFACE_COMPILE_DEFINITIONS GHOSTTY_STATIC)
else()
    FetchContent_Declare(ghostty_vt URL ${GHOSTTY_VT_RELEASE}/libghostty-vt-1.0.0.tar.gz
        URL_HASH SHA256=4a6ef758be721923e1969e7b81b443dcc92d8dd3fb5a39600c03a8817a753676)
    FetchContent_MakeAvailable(ghostty_vt)
endif()

# dear imgui and imgui_rich_md are imshell's submodules (git submodule update --init
# --recursive). imgui's core is compiled here, once; imshell only adds the backends it uses
if(WIN32) # windows.h without its min/max macros, for every target here
    add_compile_definitions(NOMINMAX)
    list(APPEND IMCODE_SSH ws2_32)
endif()
# auto-update through imshell's updater (packaging turns it on); an empty feed leaves it off.
# macos reads its feed and key from Info.plist, which imshell_app writes
option(IMCODE_UPDATER "Auto-update" OFF)
set(IMCODE_APPCAST_URL "" CACHE STRING "macos and windows: the sparkle appcast")
set(IMCODE_UPDATE_KEY "" CACHE STRING "macos and windows: the appcast's ed25519 public key (base64)")
set(IMCODE_MANIFEST_URL "" CACHE STRING "linux: the json manifest")
if(IMCODE_UPDATER)
    set(IMSHELL_UPDATER ON CACHE BOOL "" FORCE) # before imshell reads it
endif()
add_subdirectory(external/imshell)
option(IMCODE_BUILD_TESTS "Build the tests" OFF)
if(IMCODE_BUILD_TESTS)
    set(ACP_BUILD_EXAMPLES ON CACHE BOOL "" FORCE) # the echo agent
    set(ACP_HOST_BUILD_TESTS ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(external/acp-cpp)
add_subdirectory(external/acp-host)
file(GLOB IMGUI_CORE ${IMSHELL_IMGUI_DIR}/imgui*.cpp)
imshell_embed_fonts(${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts)
imshell_embed_images(${CMAKE_CURRENT_SOURCE_DIR}/assets/images) # none yet: both lookups must exist

add_executable(imcode src/main.cpp src/onboarding.cpp src/palette.cpp src/sidebar.cpp src/thread_view.cpp
               src/notify.cpp src/headless.cpp
               src/history.cpp src/diff.cpp src/git.cpp src/git_graph.cpp src/git_local.cpp src/thread.cpp src/remote.cpp src/store.cpp src/ssh.cpp src/ssh_config.cpp src/keychain.cpp
               src/terminal.cpp src/terminal_view.cpp src/files_view.cpp src/changes_view.cpp src/graph_view.cpp src/log_view.cpp src/syntax.cpp
               ${EMBEDDED_FONTS_FILE} ${EMBEDDED_IMAGES_FILE} ${IMGUI_CORE})
target_include_directories(imcode PRIVATE external/IconFontCppHeaders)
# watching folders: in process on macos (FSEvents), and on another machine through imcode-helper, a
# small program built here, embedded in the app (cmake/embed_helper.cmake) and uploaded over ssh
if(APPLE AND NOT IOS)
    add_executable(imcode-helper src/watch_helper.cpp src/watch.cpp)
    target_link_libraries(imcode-helper PRIVATE "-framework CoreServices")
    set(IMCODE_HELPER_IN $<TARGET_FILE:imcode-helper>)
    set(IMCODE_HELPER_DEP imcode-helper)
    target_link_libraries(imcode PRIVATE "-framework CoreServices")
else()
    set(IMCODE_HELPER_IN "")
    set(IMCODE_HELPER_DEP "")
endif()
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/helper_bytes.cpp
    COMMAND ${CMAKE_COMMAND} "-DIN=${IMCODE_HELPER_IN}" "-DOUT=${CMAKE_BINARY_DIR}/helper_bytes.cpp"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_helper.cmake
    DEPENDS ${IMCODE_HELPER_DEP} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_helper.cmake VERBATIM)
target_sources(imcode PRIVATE src/watch.cpp src/folder_watch.cpp ${CMAKE_BINARY_DIR}/helper_bytes.cpp)
# the version shown and compared by the updater; a test build of an update overrides it
set(IMCODE_VERSION ${PROJECT_VERSION} CACHE STRING "the version")
# the commit it was built from, shown in settings
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE IMCODE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
target_compile_definitions(imcode PRIVATE IMCODE_VERSION="${IMCODE_VERSION}" IMCODE_COMMIT="${IMCODE_COMMIT}"
    IMCODE_APPCAST_URL="${IMCODE_APPCAST_URL}" IMCODE_UPDATE_KEY="${IMCODE_UPDATE_KEY}" IMCODE_MANIFEST_URL="${IMCODE_MANIFEST_URL}")
if(IMCODE_UPDATER)
    target_compile_definitions(imcode PRIVATE IMCODE_UPDATER=1)
endif()
imshell_app(imcode NAME ImCode ID io.github.dunkbing.imcode VERSION ${IMCODE_VERSION}
            APPCAST "${IMCODE_APPCAST_URL}" PUBLIC_KEY "${IMCODE_UPDATE_KEY}")
target_link_libraries(imcode PRIVATE imshell::imshell imshell::components imshell::markdown acp::host ${IMCODE_SSH} ghostty-vt-static)

# ctest: configure with -DIMCODE_BUILD_TESTS=ON (turns on the echo agent it talks to)
if(IMCODE_BUILD_TESTS)
    enable_testing()
    add_executable(imcode-diff-test tests/diff_test.cpp src/diff.cpp)
    add_test(NAME imcode-diff COMMAND imcode-diff-test)
    if(APPLE)
        add_executable(imcode-keychain-test tests/keychain_test.cpp src/keychain.cpp)
        target_link_libraries(imcode-keychain-test PRIVATE "-framework Security" "-framework CoreFoundation")
        target_compile_features(imcode-keychain-test PRIVATE cxx_std_20)
        add_test(NAME imcode-keychain COMMAND imcode-keychain-test)
    endif()
    add_executable(imcode-watch-test tests/watch_test.cpp src/watch.cpp)
    if(APPLE)
        target_link_libraries(imcode-watch-test PRIVATE "-framework CoreServices")
    endif()
    target_include_directories(imcode-watch-test PRIVATE src)
    target_compile_features(imcode-watch-test PRIVATE cxx_std_20)
    add_test(NAME imcode-watch COMMAND imcode-watch-test)
    add_executable(imcode-git-test tests/git_test.cpp src/git.cpp src/git_graph.cpp src/git_local.cpp src/ssh.cpp src/ssh_config.cpp src/keychain.cpp)
    target_link_libraries(imcode-git-test PRIVATE acp::host ${IMCODE_SSH})
    add_test(NAME imcode-git COMMAND imcode-git-test)
    add_executable(imcode-history-test tests/history_test.cpp src/history.cpp src/ssh.cpp src/ssh_config.cpp src/keychain.cpp)
    target_link_libraries(imcode-history-test PRIVATE acp::host ${IMCODE_SSH})
    add_test(NAME imcode-history COMMAND imcode-history-test)
    add_executable(imcode-remote-test tests/remote_test.cpp src/remote.cpp)
    target_link_libraries(imcode-remote-test PRIVATE acp::host)
    add_test(NAME imcode-remote COMMAND imcode-remote-test)
    add_executable(imcode-thread-test tests/thread_test.cpp src/diff.cpp src/git.cpp src/git_graph.cpp src/git_local.cpp src/thread.cpp src/history.cpp src/remote.cpp src/store.cpp src/ssh.cpp src/ssh_config.cpp src/keychain.cpp)
    target_link_libraries(imcode-thread-test PRIVATE acp::host ${IMCODE_SSH})
    # manual: a real ssh server is needed (see the file)
    add_executable(imcode-ssh-check tests/ssh_check.cpp src/diff.cpp src/git.cpp src/git_graph.cpp src/git_local.cpp src/thread.cpp src/history.cpp src/remote.cpp src/ssh.cpp src/ssh_config.cpp src/keychain.cpp
                   src/terminal.cpp)
    target_link_libraries(imcode-ssh-check PRIVATE acp::host ${IMCODE_SSH} ghostty-vt-static)
    add_executable(imcode-terminal-test tests/terminal_test.cpp src/terminal.cpp)
    target_link_libraries(imcode-terminal-test PRIVATE ghostty-vt-static)
    add_test(NAME imcode-terminal COMMAND imcode-terminal-test)
    add_executable(imcode-syntax-test tests/syntax_test.cpp src/syntax.cpp)
    add_test(NAME imcode-syntax COMMAND imcode-syntax-test)
    add_test(NAME imcode-thread COMMAND imcode-thread-test $<TARGET_FILE:acp-echo-agent>)
    if(WIN32)
        foreach(test imcode-git-test imcode-history-test imcode-remote-test imcode-thread-test imcode-ssh-check)
            target_compile_options(${test} PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/tests/posix_env.hpp)
        endforeach()
    endif()
endif()
