# ImCode

Run coding agents (Claude Code, Codex, Gemini, any [ACP](https://agentclientprotocol.com) agent) on this machine or on another one over ssh, several at once. Written in C++ with [imshell](https://github.com/dunkbing/imshell) (Dear ImGui) and [acp-cpp](https://github.com/dunkbing/acp-cpp). See the [roadmap](docs/roadmap.md).

## Build

Needs CMake and [vcpkg](https://vcpkg.io).

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j 4
./build/imcode
```

Tests: configure with `-DIMCODE_BUILD_TESTS=ON`, then run `ctest --test-dir build`.

## Use

Add a project: a folder on this machine, or a host over ssh (a name from `~/.ssh/config` or `user@host`) and a folder there. Start a thread and pick an agent. On a remote host the agent runs on that host, in that folder.
