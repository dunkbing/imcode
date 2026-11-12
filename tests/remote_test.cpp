// the command line built for a remote agent, run here by sh instead of ssh
#include "../src/remote.hpp"
#include "acp_host/process_runner.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc > 1) { // manual: the bun install script, to run under a clean HOME
        std::printf("%s\n", Remote::bunSetup().c_str());
        return 0;
    }
    // a folder with a quote and a space under a fake home, an agent that prints where it runs
    const auto home = std::filesystem::temp_directory_path() / "imcode remote test";
    std::filesystem::create_directories(home / "it's here");
    setenv("HOME", home.c_str(), 1);
    setenv("SHELL", "/bin/sh", 1);
    AcpAgentDef def;
    def.runCmd = {"sh", "-c", "pwd; echo \"$1\"", "agent"};
    def.runArgs = {"an 'arg'"};
    const std::string command = Remote::agentCommand(def, "~/it's here");
    const ProcessResult res = ProcessRunner::run({.args = {"/bin/sh", "-c", command}});
    std::filesystem::remove_all(home);
    const std::string want = "/it's here\nan 'arg'\n";
    if (!res.success || !res.output.ends_with(want) || !Remote::agentCommand({}, "/x").empty()) {
        std::printf("FAILED: %s\n%s\n", command.c_str(), res.output.c_str());
        return 1;
    }
    if (Remote::uploadPath("a b.png", 7) != "/tmp/imcode-uploads/7-a b.png" ||
        Remote::uploadPath("x", 1) == Remote::uploadPath("x", 2)) {
        std::printf("FAILED: uploadPath\n");
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
