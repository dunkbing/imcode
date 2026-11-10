#include "history.hpp"

#include <algorithm>
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
            out.title = text.substr(0, std::min(text.find('\n'), size_t{80}));
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

    std::vector<Found> scan(int perFolder) {
        const char* home = std::getenv("HOME");
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
