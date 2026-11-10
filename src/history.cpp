#include "history.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace History {

    namespace {
        // the folder and the first thing the user typed, from the head of a session file
        bool readHead(const fs::path& file, Found& out) {
            std::ifstream in(file);
            int lines = 0;
            for (std::string line; std::getline(in, line) && lines < 400; ++lines) {
                const json row = json::parse(line, nullptr, false);
                if (!row.is_object()) {
                    continue;
                }
                if (out.cwd.empty() && row.contains("cwd") && row["cwd"].is_string()) {
                    out.cwd = row["cwd"].get<std::string>();
                }
                if (row.value("type", "") != "user" || row.value("isMeta", false) ||
                    !row.contains("message")) {
                    continue;
                }
                const json& content = row["message"].value("content", json());
                std::string text;
                if (content.is_string()) {
                    text = content.get<std::string>();
                } else if (content.is_array()) {
                    for (const auto& block : content) {
                        if (block.value("type", "") == "text") {
                            text = block.value("text", "");
                            break;
                        }
                    }
                }
                // commands and injected context start with a tag: not what the user asked
                if (text.empty() || text.front() == '<') {
                    continue;
                }
                out.title = text.substr(0, std::min(text.find('\n'), size_t{80}));
                return !out.cwd.empty();
            }
            return false;
        }
    } // namespace

    std::vector<Found> claudeCode(int perFolder) {
        std::vector<Found> out;
        const char* home = std::getenv("HOME");
        std::error_code ec;
        for (const auto& folder :
             fs::directory_iterator(fs::path(home ? home : "") / ".claude" / "projects", ec)) {
            std::vector<fs::directory_entry> files;
            for (const auto& file : fs::directory_iterator(folder.path(), ec)) {
                if (file.path().extension() == ".jsonl") {
                    files.push_back(file);
                }
            }
            std::ranges::sort(files, [](const auto& a, const auto& b) {
                std::error_code e;
                return a.last_write_time(e) > b.last_write_time(e);
            });
            int taken = 0;
            for (const auto& file : files) {
                if (taken >= perFolder) {
                    break;
                }
                Found found;
                found.agentId = "claude-code";
                found.sessionId = file.path().stem().string();
                if (!readHead(file.path(), found) || !fs::is_directory(found.cwd, ec)) {
                    continue;
                }
                // file time to unix time, by the distance from now on both clocks
                found.updatedAt = std::chrono::duration_cast<std::chrono::seconds>(
                                      (std::chrono::system_clock::now() +
                                       (file.last_write_time(ec) - fs::file_time_type::clock::now()))
                                          .time_since_epoch())
                                      .count();
                out.push_back(std::move(found));
                ++taken;
            }
        }
        return out;
    }

} // namespace History
