#pragma once

#include "git.hpp"
#include <vector>

// where the lines of a history graph run. no ui: the view draws a row from these
namespace Git {
    // one commit's row; lanes are columns, counted from the left
    struct GraphRow {
        int col = 0;                // the commit's dot
        int width = 1;             // lanes this row needs
        std::vector<int> into;     // lanes whose line ends at the dot (above it; the dot's own among them)
        std::vector<int> pass;     // lanes that run straight through the row
        std::vector<int> parents;  // lanes the dot continues into (below it), first parent first
    };
    // needs the entries newest first with parents after their children (log() gives that)
    std::vector<GraphRow> layoutGraph(const std::vector<LogEntry>& entries);
} // namespace Git
