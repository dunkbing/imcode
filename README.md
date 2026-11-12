# ImCode

Run coding agents (Claude Code, Codex, Gemini, any [ACP](https://agentclientprotocol.com) agent) on this machine or on another one over ssh, several at once. Written in C++ with [imshell](https://github.com/dunkbing/imshell) (Dear ImGui) and [acp-cpp](https://github.com/dunkbing/acp-cpp). See the [roadmap](docs/roadmap.md).

## Build

Needs CMake and [vcpkg](https://vcpkg.io). On Linux and Windows also [zig](https://ziglang.org) 0.17 or later on PATH (the terminal library builds from source there); on Windows, local git commands need Git for Windows.

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j 4
./build/imcode
```

Tests: configure with `-DIMCODE_BUILD_TESTS=ON`, then run `ctest --test-dir build`.

Releases on macOS go through imshell's `imshell_app`: `cmake --build build-release --target imcode-app` makes `ImCode.app` and `imcode-macos-arm64.zip` in the build folder, signed with `IMSHELL_SIGN_IDENTITY` and notarized with `IMSHELL_NOTARY_PROFILE` when they are in the environment (ad hoc otherwise). Configure the folder first; a feed and key turn auto-update on, and it stays off without them:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx-release -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets \
  -DIMCODE_UPDATER=ON -DIMCODE_APPCAST_URL=<appcast> -DIMCODE_UPDATE_KEY=<ed25519 public key>
```

`scripts/package-linux.sh` makes the Linux AppImage.

## Use

Add a project: a folder on this machine, or a host over ssh (a name from `~/.ssh/config` or `user@host`) and a folder there. Start a thread and pick an agent. On a remote host the agent runs on that host, in that folder.
