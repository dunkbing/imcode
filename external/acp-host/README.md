# acp-host

What an app needs around [acp-cpp](https://github.com/dunkbing/acp-cpp) to run coding agents: which agents exist, how to find or download them, and one agent session as events a UI can poll. Extracted from [DearSQL](https://github.com/dunkbing/dearsql).

| Header | Contents |
|--------|----------|
| `acp_host/agents.hpp` | `AcpAgents`: the catalog, the login shell `PATH`, the argv that launches an agent, opening a terminal for a sign-in |
| `acp_host/registry.hpp` | `AcpRegistry`: the ACP registry, binary downloads with SHA-256, a managed Bun; `AcpRegistryClient` runs them in the background |
| `acp_host/client.hpp` | `AcpClient`: spawn, initialize, session new/load, sign-in restarts; `drainEvents()` once per frame |
| `acp_host/process_runner.hpp` | `ProcessRunner::run`: a command to its exit, with a timeout |
| `acp_host/host.hpp` | `AcpHost::setWake` (wake an idle UI loop), `AsyncOp` |

```cmake
add_subdirectory(external/acp-cpp)
add_subdirectory(external/acp-host)   # after acp-cpp
target_link_libraries(app PRIVATE acp::host)
```

```cpp
AcpRegistry::setInstallRoot(dataDir / "agents");
AcpHost::setWake([] { requestFrame(); });
acp::setLogger(...); // optional

AcpClient client;
client.start(*AcpAgents::resolveInvocation(*AcpAgents::find("claude-code")), cwd, "", "", "");
client.prompt(acp::json::array({acp::textBlock("hello")})); // sent when the session opens
for (auto& ev : client.drainEvents()) { ... }               // each frame
```

Dependencies: acp-cpp, cpp-httplib with OpenSSL. Tests: `-DACP_BUILD_EXAMPLES=ON -DACP_HOST_BUILD_TESTS=ON`, then `ctest`.

MIT.
