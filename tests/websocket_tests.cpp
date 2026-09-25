#include "r2t2/inference.h"
#include "r2t2/websocket_server.h"

#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class FakeEngine final : public r2t2::InferenceEngine {
public:
    r2t2::Generation generate(
        std::span<const float>,
        std::string_view,
        std::uint32_t) override {
        return {"Hello", {}, false};
    }

    std::vector<std::int32_t> tokenize(std::string_view text) const override {
        std::vector<std::int32_t> tokens;
        for (const auto value : text) tokens.push_back(static_cast<unsigned char>(value));
        return tokens;
    }

    std::string detokenize(std::span<const std::int32_t> tokens) const override {
        std::string text;
        for (const auto token : tokens) text.push_back(static_cast<char>(token));
        return text;
    }
};

void run_native_session(std::uint16_t port) {
    ix::WebSocket client;
    client.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/asr_stream_api_v1?token=test-token");
    client.disableAutomaticReconnection();

    bool saw_connected = false;
    bool saw_delta = false;
    bool saw_final = false;
    std::string error;
    client.setOnMessageCallback([&](const ix::WebSocketMessagePtr & message) {
        if (message->type == ix::WebSocketMessageType::Open) {
            client.sendText(R"({"requestId":"integration","channels":1,"sample_rate":16000,"language":"Chinese","use_vad":false})");
        } else if (message->type == ix::WebSocketMessageType::Message) {
            if (message->str.find(R"("status":"connected")") != std::string::npos) {
                saw_connected = true;
                client.sendBinary(std::string(10240, '\0'));
                client.sendText("YOUDAO_ONETIME_ASR_STREAM_EOS");
            } else if (message->str.find(R"("status":"success")") != std::string::npos) {
                saw_delta = saw_delta || message->str.find(R"("text":"Hell")") != std::string::npos;
                saw_final = saw_final || message->str.find(R"("reset":true)") != std::string::npos;
            } else if (message->str.find(R"("status":"error")") != std::string::npos) {
                error = message->str;
            }
        } else if (message->type == ix::WebSocketMessageType::Error) {
            error = message->errorInfo.reason;
        }
    });

    const auto connection = client.connect(5);
    if (!connection.success) throw std::runtime_error(connection.errorStr);
    client.run();
    if (!error.empty()) throw std::runtime_error(error);
    if (!saw_connected || !saw_delta || !saw_final) {
        throw std::runtime_error("WebSocket session did not complete the expected protocol sequence");
    }
}

void run_rstream_session(std::uint16_t port) {
    ix::WebSocket client;
    client.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/asr?t=test-token");
    client.disableAutomaticReconnection();

    bool saw_connected = false;
    bool saw_cumulative_text = false;
    bool saw_final = false;
    std::string error;
    client.setOnMessageCallback([&](const ix::WebSocketMessagePtr & message) {
        if (message->type == ix::WebSocketMessageType::Open) {
            client.sendText(R"({"requestId":"rstream-integration","lang":"cn","booked_words":"Technical words: Tiginal","use_vad":false,"smooth":true})");
        } else if (message->type == ix::WebSocketMessageType::Message) {
            if (message->str.find(R"("status":"connected")") != std::string::npos) {
                saw_connected = true;
                client.sendBinary(std::string(10240, '\0'));
                client.sendText("YOUDAO_ASR_EOS");
            } else if (message->str.find(R"("status":"success")") != std::string::npos) {
                saw_cumulative_text = saw_cumulative_text
                    || message->str.find(R"("text":"Hell")") != std::string::npos;
                saw_final = saw_final
                    || message->str.find(R"("is_final":true)") != std::string::npos;
            } else if (message->str.find(R"("status":"error")") != std::string::npos) {
                error = message->str;
            }
        } else if (message->type == ix::WebSocketMessageType::Error) {
            error = message->errorInfo.reason;
        }
    });

    const auto connection = client.connect(5);
    if (!connection.success) throw std::runtime_error(connection.errorStr);
    client.run();
    if (!error.empty()) throw std::runtime_error(error);
    if (!saw_connected || !saw_cumulative_text || !saw_final) {
        throw std::runtime_error("WebSocket rstream session did not complete the expected protocol sequence");
    }
}

} // namespace

int main() {
    try {
        FakeEngine engine;
        r2t2::AppConfig config;
        config.port = 0;
        config.auth.mode = r2t2::AuthMode::fixed;
        config.auth.token = "test-token";
        config.debug = true;
        config.verbose = true;
        r2t2::WebSocketServer server(config, engine);
        std::ostringstream debug_log;
        auto * previous_stderr = std::cerr.rdbuf(debug_log.rdbuf());
        try {
            const auto port = server.start();
            run_native_session(port);
            run_rstream_session(port);
            run_native_session(port);
            server.stop();
            std::cerr.rdbuf(previous_stderr);
        } catch (...) {
            std::cerr.rdbuf(previous_stderr);
            throw;
        }
        const auto trace = debug_log.str();
        if (trace.find("input binary: bytes=10240 samples=5120") == std::string::npos
            || trace.find("input text: bytes=29 value=\"YOUDAO_ONETIME_ASR_STREAM_EOS\"") == std::string::npos
            || trace.find("output text: {\"msg\"") == std::string::npos) {
            throw std::runtime_error("debug mode did not trace WebSocket input and output");
        }
        std::cout << "WebSocket integration tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception & error) {
        if (std::string_view(error.what()).find("Operation not permitted") != std::string_view::npos) {
            std::cerr << "WebSocket integration test skipped: local sockets are not permitted\n";
            return 77;
        }
        std::cerr << "WebSocket integration test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
