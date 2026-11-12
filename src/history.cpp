#include "history.hpp"
#include "home.hpp"
#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace History {

    namespace {
        // the first text block of a message's content (a string, or blocks with "text")
        std::string textOf(const json& content) {
            if (content.is_string()) {
                return content.get<std::string>();
            }
            for (const auto& block : content.is_array() ? content : json::array()) {
                if (block.contains("text") && block["text"].is_string()) {
                    return block["text"].get<std::string>();
                }
            }
            return "";
        }

        // what the user asked first; injected context and commands start with a tag or a
        // heading and are not it
        bool takeTitle(const std::string& text, Found& out) {
            if (text.empty() || text.front() == '<' || text.front() == '#') {
                return false;
            }
            out.title = cut(text.substr(0, text.find('\n')), 80);
            return true;
        }

        // folder, session id and first prompt from the head of a session file
        bool readHead(const fs::path& file, Found& out) {
            std::ifstream in(file);
            int lines = 0;
            for (std::string line; std::getline(in, line) && lines < 400; ++lines) {
                const json row = json::parse(line, nullptr, false);
                if (!row.is_object()) {
                    continue;
                }
                const std::string type = row.value("type", "");
                const json payload = row.value("payload", json::object());
                if (type == "session_meta") { // codex
                    out.cwd = payload.value("cwd", "");
                    out.sessionId = payload.value("id", "");
                } else if (type == "response_item" && payload.value("role", "") == "user") {
                    if (takeTitle(textOf(payload.value("content", json())), out)) {
                        break;
                    }
                } else if (out.cwd.empty() && row.contains("cwd") && row["cwd"].is_string()) {
                    out.cwd = row["cwd"].get<std::string>(); // claude code: on every row
                }
                if (type == "user" && !row.value("isMeta", false) && row.contains("message") &&
                    takeTitle(textOf(row["message"].value("content", json())), out)) {
                    break;
                }
            }
            return !out.cwd.empty() && !out.sessionId.empty() && !out.title.empty();
        }

        long long modified(const fs::path& file) {
            // file time to unix time, by the distance from now on both clocks
            std::error_code ec;
            return std::chrono::duration_cast<std::chrono::seconds>(
                       (std::chrono::system_clock::now() +
                        (fs::last_write_time(file, ec) - fs::file_time_type::clock::now()))
                           .time_since_epoch())
                .count();
        }
    } // namespace

    std::string transcript(const std::string& agentId, const std::string& cwd, const std::string& sessionId) {
        const char* home = homeEnv();
        const fs::path base = home ? home : "";
        std::error_code ec;
        if (agentId == "claude-code") {
            // the folder's path with every character but letters and digits as "-"
            std::string folder = cwd;
            for (char& c : folder) {
                c = std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
            }
            const fs::path file = base / ".claude" / "projects" / folder / (sessionId + ".jsonl");
            return fs::exists(file, ec) ? file.string() : "";
        }
        if (agentId == "codex" && !sessionId.empty()) {
            // rollout-<time>-<session>.jsonl under a dated folder
            for (const auto& entry : fs::recursive_directory_iterator(base / ".codex" / "sessions", ec)) {
                const std::string name = entry.path().filename().string();
                if (name.ends_with(sessionId + ".jsonl")) {
                    return entry.path().string();
                }
            }
        }
        return "";
    }

    long long quietFor(const std::string& path) {
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec)) {
            return 1'000'000'000;
        }
        return std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() -
                                                                fs::last_write_time(path, ec))
            .count();
    }

    std::vector<Entry> readFrom(const std::string& agentId, const std::string& path, size_t& offset) {
        std::vector<Entry> out;
        // at most a few megabytes a call, read in one go: a long conversation (screenshots in
        // it) runs to a hundred megabytes and is taken a piece at a time
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        const auto size = static_cast<size_t>(std::max<std::streamoff>(0, in.tellg()));
        if (offset >= size) {
            return out;
        }
        std::string chunk(std::min(size - offset, maxRead), '\0');
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        chunk.resize(static_cast<size_t>(in.gcount()));
        // only complete lines: the writer may be in the middle of one
        const size_t end = chunk.rfind('\n');
        if (end == std::string::npos) {
            if (chunk.size() == maxRead) {
                offset += chunk.size(); // one enormous line (a picture): the rest of it fails to parse, and is left out
            }
            return out;
        }
        offset += end + 1;
        const auto shown = [](const std::string& text) { // injected context starts with a tag or a heading
            return !text.empty() && text.front() != '<' && !text.starts_with("# AGENTS.md");
        };
        for (size_t at = 0; at <= end;) {
            const size_t stop = chunk.find('\n', at);
            const json row = json::parse(chunk.substr(at, stop - at), nullptr, false);
            at = stop + 1;
            if (!row.is_object()) {
                continue;
            }
            const std::string type = row.value("type", "");
            if (agentId == "codex" && type == "response_item") {
                const json p = row.value("payload", json::object());
                const std::string kind = p.value("type", "");
                if (kind == "message") {
                    const std::string role = p.value("role", "");
                    std::string text;
                    for (const auto& block : p.value("content", json::array())) {
                        text += block.value("text", "");
                    }
                    if (role == "user" && shown(text)) {
                        out.push_back({Entry::Kind::User, text});
                    } else if (role == "assistant" && !text.empty()) {
                        out.push_back({Entry::Kind::Assistant, text});
                    }
                } else if (kind == "function_call" || kind == "custom_tool_call" || kind == "local_shell_call") {
                    std::string title = p.value("name", "tool");
                    const std::string args = p.contains("arguments") && p["arguments"].is_string() ? p["arguments"].get<std::string>()
                                             : p.contains("input") && p["input"].is_string()       ? p["input"].get<std::string>()
                                                                                                   : "";
                    if (!args.empty()) {
                        title += " " + cut(args.substr(0, args.find('\n')), 120);
                    }
                    out.push_back({Entry::Kind::Tool, title});
                }
            } else if (agentId == "claude-code" && (type == "user" || type == "assistant") &&
                       !row.value("isMeta", false) && row.contains("message")) {
                const json content = row["message"].value("content", json());
                if (content.is_string()) {
                    if (type == "user" && shown(content.get<std::string>())) {
                        out.push_back({Entry::Kind::User, content.get<std::string>()});
                    }
                    continue;
                }
                for (const auto& block : content.is_array() ? content : json::array()) {
                    const std::string kind = block.value("type", "");
                    if (kind == "text" && block.contains("text")) {
                        const std::string text = block["text"].get<std::string>();
                        if (type == "assistant") {
                            out.push_back({Entry::Kind::Assistant, text});
                        } else if (shown(text)) {
                            out.push_back({Entry::Kind::User, text});
                        }
                    } else if (kind == "tool_use") {
                        std::string title = block.value("name", "tool");
                        const json input = block.value("input", json::object());
                        for (const char* key : {"command", "file_path", "path", "pattern", "description"}) {
                            if (input.contains(key) && input[key].is_string()) {
                                const std::string v = input[key].get<std::string>();
                                title += " " + cut(v.substr(0, v.find('\n')), 120);
                                break;
                            }
                        }
                        out.push_back({Entry::Kind::Tool, title});
                    }
                }
            }
        }
        return out;
    }

    std::vector<Found> scan(int perFolder) {
        const char* home = homeEnv();
        const fs::path base = home ? home : "";
        std::error_code ec;
        // every session file with its agent, newest first
        std::vector<std::pair<fs::path, const char*>> files;
        for (const auto& entry :
             fs::recursive_directory_iterator(base / ".claude" / "projects", ec)) {
            // a folder's own sessions only: deeper files are subagent logs
            if (entry.path().extension() == ".jsonl" &&
                entry.path().parent_path().parent_path() == base / ".claude" / "projects") {
                files.emplace_back(entry.path(), "claude-code");
            }
        }
        for (const auto& entry : fs::recursive_directory_iterator(base / ".codex" / "sessions", ec)) {
            if (entry.path().extension() == ".jsonl") {
                files.emplace_back(entry.path(), "codex");
            }
        }
        std::vector<std::pair<long long, size_t>> order;
        for (size_t i = 0; i < files.size(); ++i) {
            order.emplace_back(modified(files[i].first), i);
        }
        std::ranges::sort(order, std::greater<>());

        std::vector<Found> out;
        std::map<std::string, int> taken; // per agent and folder
        for (const auto& [when, i] : order) {
            Found found;
            found.agentId = files[i].second;
            found.updatedAt = when;
            if (found.agentId == "claude-code") {
                found.sessionId = files[i].first.stem().string();
            }
            if (!readHead(files[i].first, found) || !fs::is_directory(found.cwd, ec) ||
                ++taken[found.agentId + "\n" + found.cwd] > perFolder) {
                continue;
            }
            out.push_back(std::move(found));
        }
        return out;
    }

} // namespace History
