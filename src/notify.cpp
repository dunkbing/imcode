#include "notify.hpp"

#include "imshell/notifications.hpp"

void notify(const std::string& id, const std::string& title, const std::string& body) {
    namespace n = imshell::notifications;
    // the system asks the first time (macos), then answers from its settings. a dev build is
    // no .app and gets none: build imcode-app to see them
    n::RequestPermission([=](n::Permission p) {
        if (p == n::Permission::Granted) {
            n::Show({.id = id, .title = title, .body = body});
        }
    });
}
