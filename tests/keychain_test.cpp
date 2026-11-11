// a secret written under a service of the test's own reads back, is replaced, and is removed
#include "../src/keychain.hpp"
#include <cstdio>

int main() {
    const char* service = "imcode-keychain-test";
    Keychain::remove(service, "me@box");
    bool ok = Keychain::read(service, "me@box").empty();
    ok &= Keychain::write(service, "me@box", "s3cret");
    ok &= Keychain::read(service, "me@box") == "s3cret";
    ok &= Keychain::write(service, "me@box", "changed"); // replaces
    ok &= Keychain::read(service, "me@box") == "changed";
    ok &= Keychain::read(service, "other@box").empty();
    Keychain::remove(service, "me@box");
    ok &= Keychain::read(service, "me@box").empty();
    std::printf(ok ? "ok\n" : "FAILED\n");
    return ok ? 0 : 1;
}
