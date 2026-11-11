cmake_minimum_required(VERSION 3.23)
project(acp-host VERSION 0.1.0 LANGUAGES CXX)

# the app adds acp-cpp itself (add_subdirectory) before this, so both share one copy
if(NOT TARGET acp::acp)
    message(FATAL_ERROR "acp-host: add acp-cpp (target acp::acp) before acp-host")
endif()
find_package(httplib CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(acp_host STATIC
    src/agents.cpp
    src/client.cpp
    src/host.cpp
    src/process_runner.cpp
    src/registry.cpp
)
add_library(acp::host ALIAS acp_host)
target_compile_features(acp_host PUBLIC cxx_std_20)
target_include_directories(acp_host PUBLIC include)
target_link_libraries(acp_host PUBLIC acp::acp
                               PRIVATE httplib::httplib OpenSSL::SSL OpenSSL::Crypto)

# needs acp-cpp's echo agent: configure with -DACP_BUILD_EXAMPLES=ON as well
option(ACP_HOST_BUILD_TESTS "Build the tests (ctest)" OFF)
if(ACP_HOST_BUILD_TESTS AND TARGET acp-echo-agent)
    enable_testing()
    add_executable(acp-host-echo-test tests/echo_test.cpp)
    target_link_libraries(acp-host-echo-test PRIVATE acp::host)
    add_test(NAME acp-host-echo COMMAND acp-host-echo-test $<TARGET_FILE:acp-echo-agent>)
endif()
