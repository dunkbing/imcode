# imcode

Run coding agents (Claude Code, Codex, Gemini, any [ACP](https://agentclientprotocol.com) agent) in folders on this machine or over ssh, several at once. C++ with [mydear](https://github.com/dunkbing/mydear) (Dear ImGui) and [acp-cpp](https://github.com/dunkbing/acp-cpp). See the [roadmap](docs/roadmap.md).

## Build

```sh
git submodule update --init
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j 4
./build/imcode
```

Tests (no window): add `-DIMCODE_BUILD_TESTS=ON`, then `ctest --test-dir build`.

## Use

"Open folder": a folder on this machine, or a host over ssh (a name from `~/.ssh/config` or `user@host`) and then a folder there. Press + beside the folder and pick an agent. On a remote host the agent runs there: it must be signed in, with `bun` or `npx` on the login `PATH`.

`build/imcode-ssh-check <host> <folder> <acp-echo-agent on the host>` checks the ssh path against a real server.
