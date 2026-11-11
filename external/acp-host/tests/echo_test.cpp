// AcpClient against acp-cpp's echo agent: handshake, one prompt, the reply, the turn's end
// (the agent echoes each word followed by a space)
#include "acp_host/client.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char** argv) {
    assert(argc == 2); // path of acp-echo-agent
    AcpClient client;
    const auto [ok, err] = client.start({argv[1]}, ".", "", "", "");
    assert(ok && err.empty());

    client.prompt(acp::json::array({acp::textBlock("hello")})); // held until the session opens
    bool ended = false;
    std::string reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ended && std::chrono::steady_clock::now() < deadline) {
        client.tick();
        for (const auto& ev : client.drainEvents()) {
            if (ev.type == AcpEvent::Type::MessageDelta) {
                reply += ev.text;
            } else if (ev.type == AcpEvent::Type::TurnEnded) {
                ended = true;
            } else if (ev.type == AcpEvent::Type::Error || ev.type == AcpEvent::Type::Exited) {
                std::printf("FAILED: %s\n", ev.text.c_str());
                return 1;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client.stop();
    if (!ended || reply != "hello ") {
        std::printf("FAILED: ended=%d reply=%s\n", ended, reply.c_str());
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
