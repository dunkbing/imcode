#include "git_graph.hpp"

#include <algorithm>

namespace Git {
    std::vector<GraphRow> layoutGraph(const std::vector<LogEntry>& entries) {
        std::vector<GraphRow> rows;
        rows.reserve(entries.size());
        std::vector<std::string> lanes; // the commit each lane waits for; empty: a free lane
        const auto laneOf = [&](const std::string& hash) {
            const auto at = std::ranges::find(lanes, hash);
            return at == lanes.end() ? -1 : static_cast<int>(at - lanes.begin());
        };
        const auto freeLane = [&] {
            const int at = laneOf("");
            if (at >= 0) {
                return at;
            }
            lanes.emplace_back();
            return static_cast<int>(lanes.size()) - 1;
        };
        for (const auto& entry : entries) {
            GraphRow row;
            for (int i = 0; i < static_cast<int>(lanes.size()); ++i) {
                if (lanes[static_cast<size_t>(i)] == entry.hash) {
                    row.into.push_back(i);
                } else if (!lanes[static_cast<size_t>(i)].empty()) {
                    row.pass.push_back(i);
                }
            }
            // a commit nothing waits for (a branch tip) takes a lane of its own
            row.col = row.into.empty() ? freeLane() : row.into.front();
            int width = static_cast<int>(lanes.size());
            for (const int i : row.into) {
                lanes[static_cast<size_t>(i)].clear();
            }
            for (size_t p = 0; p < entry.parents.size(); ++p) {
                const std::string& parent = entry.parents[p];
                int lane = laneOf(parent);
                if (lane < 0) { // the first parent carries on in the dot's lane
                    lane = p == 0 ? row.col : freeLane();
                    lanes[static_cast<size_t>(lane)] = parent;
                }
                row.parents.push_back(lane);
            }
            width = std::max(width, static_cast<int>(lanes.size()));
            while (!lanes.empty() && lanes.back().empty()) {
                lanes.pop_back();
            }
            row.width = std::max(1, width);
            rows.push_back(std::move(row));
        }
        return rows;
    }
} // namespace Git
