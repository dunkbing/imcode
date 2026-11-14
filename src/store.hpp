#pragma once

#include <map>
#include "thread.hpp"
#include <filesystem>
#include <memory>
#include <vector>

// what survives a launch: the opened folders and the thread list. only what names a
// conversation is kept: the agent holds the transcript and replays it on session/load.
// shortcut: one json file; a table with row versions comes with the sync protocol
namespace Store {
    // a folder threads are started in; host empty = this machine
    struct Project {
        std::string host, cwd;
        bool operator==(const Project&) const = default;
    };
    struct Data {
        bool onboarded = false;
        std::vector<std::string> agents; // ids turned on: what the pickers offer
        std::string theme = "tokyo-night"; // an imshell theme id
        // what the system tells the user: a thread needs an answer, a turn finished, and whether it
        // also does so for a thread that is not the open one while the window is in front
        bool notifyAnswer = true, notifyFinished = true, notifyOthers = true;
        // panel sizes as the user dragged them: the list's width, the chat's share beside the
        // files, the file tree's width, the chat's share above the terminal
        // pixels; 0: not sized yet (the panels share the room)
        float sidebarWidth = 264, filesWidth = 0, treeWidth = 220, terminalHeight = 240;
        bool sidebarHidden = false; // the thread list is hidden (title-bar toggle)
        std::string lastSession;         // the thread that was open: opened again at launch
        Project filter; // the project the sidebar lists; empty cwd: all of them
        // per agent, the model, mode, ... last picked: what its next new thread starts with
        std::map<std::string, std::map<std::string, std::string>> defaults;
        std::vector<std::string> machines; // ssh hosts added in Settings, besides those a project is on
        std::string machine; // the working machine: an ssh host, empty = this computer
        std::vector<std::string> importAsked; // hosts whose earlier sessions were offered once
        std::vector<Project> projects;
        std::vector<std::unique_ptr<Thread>> threads;
    };

    Data load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file, const Data& data);
} // namespace Store
