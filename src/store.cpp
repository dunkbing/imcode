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
        out.onboarded = root.value("onboarded", false);
        out.agents = root.value("agents", std::vector<std::string>{});
        out.lastSession = root.value("last", "");
        out.filter = {root.value("filterHost", ""), root.value("filterCwd", "")};
        out.theme = root.value("theme", out.theme);
        out.notifyAnswer = root.value("notifyAnswer", true);
        out.notifyFinished = root.value("notifyFinished", true);
        out.notifyOthers = root.value("notifyOthers", true);
        out.machines = root.value("machines", std::vector<std::string>{});
        out.machine = root.value("machine", "");
        out.importAsked = root.value("importAsked", std::vector<std::string>{});
        out.defaults = root.value("defaults", out.defaults);
        if (const auto layout = root.value("layout", std::vector<float>{}); layout.size() == 4) {
            out.sidebarWidth = layout[0];
            out.filesWidth = layout[1] > 1 ? layout[1] : 0; // older builds kept shares (0..1) here
            out.treeWidth = layout[2];
            out.terminalHeight = layout[3] > 1 ? layout[3] : out.terminalHeight;
        }
        for (const auto& row : root.value("projects", json::array())) {
            addProject({row.value("host", ""), row.value("cwd", "")});
        }
        for (const auto& row : root.value("threads", json::array())) {
            auto thread = std::make_unique<Thread>(row.value("cwd", ""), row.value("agent", ""),
                                                   row.value("host", ""));
            thread->title = row.value("title", "");
            thread->sessionId = row.value("session", "");
            thread->root = row.value("root", "");
            thread->renamed = row.value("renamed", false);
            thread->archived = row.value("archived", false);
            thread->updatedAt = row.value("updated", 0LL);
            addProject({thread->host, thread->projectDir()}); // a thread is never without its folder
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
                               {"session", t->sessionId},
                               {"root", t->root},
                               {"renamed", t->renamed},
                               {"archived", t->archived},
                               {"updated", t->updatedAt}});
        }
        // written beside and renamed: a crash mid-write must not lose the list
        const auto tmp = std::filesystem::path(file).concat(".tmp");
        std::ofstream(tmp) << json{{"onboarded", data.onboarded},
                                   {"agents", data.agents},
                                   {"last", data.lastSession},
                                   {"filterHost", data.filter.host},
                                   {"filterCwd", data.filter.cwd},
                                   {"theme", data.theme},
                                   {"notifyAnswer", data.notifyAnswer},
                                   {"notifyFinished", data.notifyFinished},
                                   {"notifyOthers", data.notifyOthers},
                                   {"machines", data.machines},
                                   {"machine", data.machine},
                                   {"importAsked", data.importAsked},
                                   {"defaults", data.defaults},
                                   {"layout", {data.sidebarWidth, data.filesWidth, data.treeWidth, data.terminalHeight}},
                                   {"projects", projects},
                                   {"threads", threads}}.dump(2) << "\n";
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
    }

} // namespace Store
