// a cut copy of the trace log still starts with the env lines
#include "../src/trace.hpp"
#include <cstdio>
#include <filesystem>

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "imcode-trace-test";
    std::filesystem::remove_all(dir);
    trace::open(dir);
    trace::env("app=Test version=1");
    for (int i = 0; i < 200; ++i) {
        trace::event("filler", "line number " + std::to_string(i));
    }
    const std::string cut = trace::tail(300), whole = trace::tail(1 << 20);
    const bool ok = cut.starts_with("env  app=Test") && cut.find("(the log was cut") != std::string::npos && cut.size() < 600 &&
                    cut.find("line number 199") != std::string::npos && whole.find("env  app=Test") != std::string::npos &&
                    whole.find("was cut") == std::string::npos;
    std::filesystem::remove_all(dir);
    std::printf("%s\n", ok ? "ok" : "FAILED: trace tail");
    return ok ? 0 : 1;
}
