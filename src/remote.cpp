#include "remote.hpp"

namespace Remote {

    namespace {
        std::string quote(const std::string& s) {
            std::string out = "'";
            for (const char c : s) {
                out += c == '\'' ? std::string("'\\''") : std::string(1, c);
            }
            return out + "'";
        }

        // `cd` into dir; "~/x" must stay outside the quotes to expand
        std::string cd(const std::string& dir) {
            if (dir.empty() || dir == "~") {
                return "cd";
            }
            if (dir.starts_with("~/")) {
                return "cd \"$HOME\"/" + quote(dir.substr(2));
            }
            return "cd " + quote(dir);
        }

        // posix script under sh, started from the login shell for its PATH: the login
        // shell itself may be fish, which cannot run the script
        std::string viaLoginShell(const std::string& script) {
            return "exec $SHELL -lc " + quote("exec sh -c " + quote(script));
        }
    } // namespace

    std::string agentCommand(const AcpAgentDef& def, const std::string& dir) {
        std::string args;
        for (const auto& arg : def.runArgs) {
            args += " " + quote(arg);
        }
        std::string run;
        if (!def.npmPackage.empty()) {
            const std::string pkg = quote(def.npmPackage) + args;
            run = "if command -v bun >/dev/null 2>&1; then exec bun x --bun " + pkg +
                  "; else exec npx -y " + pkg + "; fi";
        } else if (!def.runCmd.empty()) {
            run = "exec";
            for (const auto& part : def.runCmd) {
                run += " " + quote(part);
            }
            run += args;
        } else {
            return "";
        }
        return viaLoginShell(cd(dir) + " && " + run);
    }

} // namespace Remote
