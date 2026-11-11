#include "remote.hpp"

#include "acp_host/registry.hpp"

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

        // what the user's own terminal would find: the PATH of an interactive login shell
        // (tools are often added in .zshrc, which a plain login shell skips), then the usual
        // places. stdin is the protocol's: the shell asked for its PATH must not read it.
        // shortcut: a fish user's PATH comes back space-separated and is ignored; the usual
        // places still apply
        const char* const path =
            "p=$(\"${SHELL:-sh}\" -ilc 'printf \"\\n__t3path__%s\\n\" \"$PATH\"' </dev/null 2>/dev/null"
            " | sed -n 's/^__t3path__//p' | tail -1); case \"$p\" in */*:*) PATH=\"$p\";; esac; "
            "PATH=\"$HOME/.bun/bin:$PATH:$HOME/.imcode/bun:$HOME/.local/bin:/opt/homebrew/bin:/usr/local/bin\"; "
            "export PATH; ";
    } // namespace

    std::string bunSetup() {
        std::string base = AcpRegistry::bunReleaseUrl();
        base.pop_back(); // the script adds its own slashes
        // everything it prints goes to stderr: stdout is the protocol's
        return "if ! command -v bun >/dev/null 2>&1; then ( d=\"$HOME/.imcode/bun\"; "
               "os=$(uname -s | tr '[:upper:]' '[:lower:]'); m=$(uname -m); "
               "case \"$m\" in x86_64|amd64) m=x64;; arm64|aarch64) m=aarch64;; esac; z=\"bun-$os-$m\"; "
               "mkdir -p \"$d\" && cd \"$d\" && curl -fsSL -o bun.zip " + quote(base) + "\"/$z.zip\" && "
               "curl -fsSL -o sums " + quote(base + "/SHASUMS256.txt") + " && "
               "want=$(sed -n \"s/  $z.zip\\$//p\" sums) && "
               "got=$( (sha256sum bun.zip 2>/dev/null || shasum -a 256 bun.zip) | cut -d' ' -f1) && "
               "[ -n \"$want\" ] && [ \"$want\" = \"$got\" ] && unzip -oq bun.zip && mv \"$z/bun\" bun && "
               "rm -rf \"$z\" bun.zip sums ) >&2 2>&1 || "
               "{ echo 'imcode: could not install bun here (it needs curl and unzip)' >&2; exit 127; }; fi; ";
    }

    std::string agentCommand(const AcpAgentDef& def, const std::string& dir) {
        std::string args;
        for (const auto& arg : def.runArgs) {
            args += " " + quote(arg);
        }
        std::string run;
        if (!def.npmPackage.empty()) {
            // bun runs npm agents: the user's, else one the app keeps under ~/.imcode/bun
            run = bunSetup() + cd(dir) + " && exec bun x --bun " + quote(def.npmPackage) + args;
        } else if (!def.runCmd.empty()) {
            run = cd(dir) + " && exec";
            for (const auto& part : def.runCmd) {
                run += " " + quote(part);
            }
            run += args;
        } else {
            return "";
        }
        // under sh whatever the login shell is (fish cannot run this script)
        return "exec sh -c " + quote(path + run);
    }

} // namespace Remote
