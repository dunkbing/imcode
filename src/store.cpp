#include "store.hpp"

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

namespace Store {

    Data load(const std::filesystem::path& file) {
        Data out;
        std::ifstream in(file);
        const json root = json::parse(in, nullptr, false); // a broken file is an empty list
        if (!root.is_object()) {
            return out;
        }
        auto addProject = [&](Project p) {
            if (!p.cwd.empty() && std::ranges::find(out.projects, p) == out.projects.end()) {
                out.projects.push_back(std::move(p));
            }
        };
        for (const auto& row : root.value("projects", json::array())) {
            addProject({row.value("host", ""), row.value("cwd", "")});
        }
        for (const auto& row : root.value("threads", json::array())) {
            auto thread = std::make_unique<Thread>(row.value("cwd", ""), row.value("agent", ""),
                                                   row.value("host", ""));
            thread->title = row.value("title", "");
            thread->sessionId = row.value("session", "");
            addProject({thread->host, thread->cwd}); // a thread is never without its folder
            out.threads.push_back(std::move(thread));
        }
        return out;
    }

    void save(const std::filesystem::path& file, const Data& data) {
        json projects = json::array();
        for (const auto& p : data.projects) {
            projects.push_back({{"host", p.host}, {"cwd", p.cwd}});
        }
        json threads = json::array();
        for (const auto& t : data.threads) {
            threads.push_back({{"cwd", t->cwd},
                               {"agent", t->agentId},
                               {"host", t->host},
                               {"title", t->title},
                               {"session", t->sessionId}});
        }
        // written beside and renamed: a crash mid-write must not lose the list
        const auto tmp = std::filesystem::path(file).concat(".tmp");
        std::ofstream(tmp) << json{{"projects", projects}, {"threads", threads}}.dump(2) << "\n";
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
    }

} // namespace Store
