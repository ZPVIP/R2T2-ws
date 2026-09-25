#include "r2t2/config.h"
#include "r2t2/inference.h"
#include "r2t2/protocol.h"
#include "r2t2/websocket_server.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) {
    stop_requested = 1;
}

} // namespace

int main(int argc, char ** argv) {
    try {
        const auto config = r2t2::parse_arguments(argc, argv);
        if (config.mode == r2t2::RunMode::help) {
            std::cout << r2t2::help_text();
            return EXIT_SUCCESS;
        }
        if (config.mode == r2t2::RunMode::version) {
            std::cout << "r2t2-ws " << R2T2_VERSION << '\n';
            return EXIT_SUCCESS;
        }

        r2t2::BackendRuntime runtime(config.verbose);
        if (config.mode == r2t2::RunMode::probe) {
            std::cout << r2t2::protocol::probe_json(r2t2::probe_runtime()) << '\n';
            return EXIT_SUCCESS;
        }

        const auto load_started = std::chrono::steady_clock::now();
        auto engine = r2t2::load_inference_engine(*config.models, config.inference);
        const auto load_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - load_started).count();
        std::cerr << "Model loaded in " << load_ms << " ms\n\n";

        r2t2::WebSocketServer server(config, *engine);
        const auto port = server.start();
        const auto native_url = r2t2::protocol::websocket_url(
            config.host, port, config.endpoint, server.auth_token());
        const auto rstream_url = r2t2::protocol::websocket_url(
            config.host, port, r2t2::protocol::rstream_endpoint, server.auth_token(), "t");
        std::cout << r2t2::protocol::ready_json(
            config.host, port, config.endpoint, server.auth_token()) << "\n\n" << std::flush;
        std::cerr << "Native WebSocket URL:\n" << native_url
                  << "\n\nRStream-compatible WebSocket URL:\n" << rstream_url
                  << "\n\nServer ready\n";

        std::signal(SIGINT, request_stop);
#if defined(SIGTERM)
        std::signal(SIGTERM, request_stop);
#endif
        while (!stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        server.stop();
        return EXIT_SUCCESS;
    } catch (const std::exception & error) {
        std::cerr << "r2t2-ws: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
