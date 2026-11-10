cmake_minimum_required(VERSION 3.23)
project(imcode VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(libssh2 CONFIG REQUIRED)
set(IMCODE_SSH $<IF:$<TARGET_EXISTS:libssh2::libssh2_shared>,libssh2::libssh2_shared,libssh2::libssh2_static>)

# imgui's core is compiled here, once; mydear only adds the backends it uses
set(MYDEAR_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
add_subdirectory(external/mydear)
option(IMCODE_BUILD_TESTS "Build the tests" OFF)
if(IMCODE_BUILD_TESTS)
    set(ACP_BUILD_EXAMPLES ON CACHE BOOL "" FORCE) # the echo agent
    set(ACP_HOST_BUILD_TESTS ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(external/acp-cpp)
add_subdirectory(external/acp-host)
file(GLOB IMGUI_CORE ${MYDEAR_IMGUI_DIR}/imgui*.cpp)

add_executable(imcode src/main.cpp src/thread.cpp src/remote.cpp src/store.cpp src/ssh.cpp src/ssh_config.cpp ${IMGUI_CORE})
target_link_libraries(imcode PRIVATE mydear::mydear mydear::components acp::host ${IMCODE_SSH})

# ctest: configure with -DIMCODE_BUILD_TESTS=ON (turns on the echo agent it talks to)
if(IMCODE_BUILD_TESTS)
    enable_testing()
    add_executable(imcode-remote-test tests/remote_test.cpp src/remote.cpp)
    target_link_libraries(imcode-remote-test PRIVATE acp::host)
    add_test(NAME imcode-remote COMMAND imcode-remote-test)
    add_executable(imcode-thread-test tests/thread_test.cpp src/thread.cpp src/remote.cpp src/store.cpp src/ssh.cpp src/ssh_config.cpp)
    target_link_libraries(imcode-thread-test PRIVATE acp::host ${IMCODE_SSH})
    # manual: a real ssh server is needed (see the file)
    add_executable(imcode-ssh-check tests/ssh_check.cpp src/thread.cpp src/remote.cpp src/ssh.cpp src/ssh_config.cpp)
    target_link_libraries(imcode-ssh-check PRIVATE acp::host ${IMCODE_SSH})
    add_test(NAME imcode-thread COMMAND imcode-thread-test $<TARGET_FILE:acp-echo-agent>)
endif()
