// Lightweight REST server using Boost.Beast for simple RPC callbacks.
#pragma once

#include <string>
#include <thread>
#include <atomic>

class RestServer {
public:
    // Start listening on the provided address and port (e.g. "127.0.0.1", 8080).
    RestServer(const std::string& address = "127.0.0.1", unsigned short port = 8080);
    ~RestServer();

    // Non-copyable
    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    // Stop the server (blocks until stopped)
    void stop();

private:
    void run();

    std::string m_address;
    unsigned short m_port;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

// C-style helpers to control a single global RestServer instance from the
// main application. Call `start_rest_server(addr, port)` from your app init
// and `stop_rest_server()` during shutdown.
void start_rest_server(const char* address = "127.0.0.1", unsigned short port = 8080);
void stop_rest_server();
