#pragma once

#include <string>

// a system notification (imshell's), for what happens while the window is in the background.
// id: a thread's session id, reported back when the notification is clicked. any thread
void notify(const std::string& id, const std::string& title, const std::string& body);
