#pragma once

#include <string>

// a system notification (a banner), for what happens while the window is in the background.
// returns at once: the helper runs on its own thread
void notify(const std::string& title, const std::string& body);
