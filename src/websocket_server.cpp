#include "r2t2/websocket_server.h"

#include "r2t2/protocol.h"
#include "r2t2/session.h"

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace r2t2 {
namespace {

struct RequestTarget {
    std::string path;
    std::string token;
};

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto high = hex_value(value[i + 1]);
            const auto low = hex_value(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return decoded;
}

RequestTarget parse_target(std::string_view uri) {
    const auto query_position = uri.find('?');
    RequestTarget result{std::string(uri.substr(0, query_position)), {}};
    if (query_position == std::string_view::npos) {
        return result;
    }
    auto query = uri.substr(query_position + 1);
    while (!query.empty()) {
        const auto separator = query.find('&');
        const auto item = query.substr(0, separator);
        const auto equal = item.find('=');
        const auto key = percent_decode(item.substr(0, equal));
        if (key == "token" || key == "t") {
            result.token = equal == std::string_view::npos
                ? std::string{}
                : percent_decode(item.substr(equal + 1));
        }
        if (separator == std::string_view::npos) break;
        query.remove_prefix(separator + 1);
    }
    return result;
}

std::string quote(std::string_view value) {
    return nlohmann::json(std::string(value)).dump();
}

std::string hex_preview(std::string_view bytes) {
    constexpr std::size_t preview_size = 16;
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    const auto count = std::min(bytes.size(), preview_size);
    for (std::size_t index = 0; index < count; ++index) {
        output << std::setw(2)
               << static_cast<unsigned int>(static_cast<unsigned char>(bytes[index]));
    }
    if (bytes.size() > preview_size) output << "...";
    return output.str();
}

} // namespace

struct WebSocketServer::Impl {
    struct Connection {
        Impl & owner;
        std::weak_ptr<ix::WebSocket> socket;
        std::atomic_bool admitted = false;
        bool connected_sent = false;
        protocol::WireDialect dialect = protocol::WireDialect::native;
        std::string request_id = protocol::make_request_id();
        std::string committed_text;
        StreamingSettings settings;
        std::unique_ptr<R2T2Session> session;

        Connection(Impl & owner_value, std::weak_ptr<ix::WebSocket> socket_value)
            : owner(owner_value), socket(std::move(socket_value)), settings(owner.config.streaming) {}

        ~Connection() { release(); }

        void release() noexcept {
            if (admitted.exchange(false)) {
                owner.busy.store(false);
            }
        }

        void log(std::string_view message) const {
            if (owner.config.verbose) {
                std::cerr << "connection " << request_id << ": " << message << '\n';
            }
        }

        void trace(std::string_view direction, std::string_view message) const {
            if (owner.config.debug) {
                std::cerr << "debug connection " << request_id << ' ' << direction
                          << ": " << message << '\n';
            }
        }

        void send_text(const std::string & text) {
            trace("output text", text);
            if (const auto web_socket = socket.lock()) {
                web_socket->sendText(text);
            }
        }

        void fail(std::string_view code, std::string_view message, std::uint16_t close_code) {
            send_text(protocol::error_json(request_id, code, message));
            log(std::string(code) + ": " + std::string(message));
            release();
            if (const auto web_socket = socket.lock()) {
                trace(
                    "output close",
                    "code=" + std::to_string(close_code) + " reason=" + quote(code));
                web_socket->close(close_code, std::string(code));
            }
        }

        void ensure_session() {
            if (!session) {
                session = std::make_unique<R2T2Session>(owner.engine, settings);
            }
            if (!connected_sent) {
                send_text(protocol::connected_json(request_id));
                connected_sent = true;
            }
        }

        void send_update(const RecognitionUpdate & update) {
            committed_text += update.text_delta;
            send_text(protocol::success_json(dialect, request_id, update, committed_text));
        }

        void on_open(const ix::WebSocketMessage & message) {
            const auto target = parse_target(message.openInfo.uri);
            trace(
                "input open",
                "path=" + quote(target.path) + " token=" + (target.token.empty() ? "absent" : "present"));
            if (target.path == protocol::rstream_endpoint) {
                dialect = protocol::WireDialect::rstream;
            } else if (target.path != owner.config.endpoint) {
                fail(
                    "INVALID_PATH",
                    "expected WebSocket path " + owner.config.endpoint
                        + " or " + std::string(protocol::rstream_endpoint),
                    1008);
                return;
            }
            if (!owner.auth_token.empty() || owner.config.auth.mode == AuthMode::fixed) {
                if (target.token != owner.auth_token) {
                    fail("UNAUTHORIZED", "missing or invalid local authentication token", 1008);
                    return;
                }
            }
            bool expected = false;
            if (!owner.busy.compare_exchange_strong(expected, true)) {
                fail("BUSY", "another ASR session is active", 1013);
                return;
            }
            admitted.store(true);
            log("accepted");
        }

        void on_text(std::string_view text) {
            if (!admitted.load()) return;
            trace("input text", "bytes=" + std::to_string(text.size()) + " value=" + quote(text));
            if (protocol::is_eos_command(text)) {
                ensure_session();
                if (!session->has_audio()) {
                    fail("EOS_BEFORE_AUDIO", "EOS received before any audio", 1008);
                    return;
                }
                const auto update = session->finish();
                send_update(update);
                release();
                if (const auto web_socket = socket.lock()) {
                    trace("output close", "code=1000 reason=\"ASR complete\"");
                    web_socket->close(1000, "ASR complete");
                }
                return;
            }
            if (session || connected_sent) {
                fail("UNEXPECTED_COMMAND", "only the EOS text command is accepted after streaming begins", 1008);
                return;
            }
            const auto header = protocol::parse_header(text, settings, dialect);
            request_id = header.request_id;
            settings.language = header.language;
            settings.context = header.context;
            settings.mode = header.mode;
            settings.smooth = header.smooth;
            ensure_session();
        }

        void on_binary(const std::string & bytes) {
            if (!admitted.load()) return;
            ensure_session();
            const auto view = std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(bytes.data()), bytes.size());
            const auto samples = protocol::parse_pcm_s16le(view);
            trace(
                "input binary",
                "bytes=" + std::to_string(bytes.size())
                    + " samples=" + std::to_string(samples.size())
                    + " prefix=" + hex_preview(bytes));
            for (const auto & update : session->push_pcm(samples)) {
                send_update(update);
            }
        }

        void on_message(const ix::WebSocketMessagePtr & message) {
            try {
                switch (message->type) {
                    case ix::WebSocketMessageType::Open:
                        on_open(*message);
                        break;
                    case ix::WebSocketMessageType::Message:
                        if (message->binary) on_binary(message->str);
                        else on_text(message->str);
                        break;
                    case ix::WebSocketMessageType::Close:
                        trace(
                            "input close",
                            "code=" + std::to_string(message->closeInfo.code)
                                + " reason=" + quote(message->closeInfo.reason));
                        log("closed");
                        release();
                        break;
                    case ix::WebSocketMessageType::Error:
                        log("transport error: " + message->errorInfo.reason);
                        release();
                        break;
                    default:
                        break;
                }
            } catch (const std::invalid_argument & error) {
                const auto code = message->binary ? "INVALID_AUDIO" : "INVALID_HEADER";
                fail(code, error.what(), message->binary ? 1003 : 1008);
            } catch (const std::exception & error) {
                fail("INFERENCE_FAILED", error.what(), 1011);
            }
        }
    };

    AppConfig config;
    InferenceEngine & engine;
    std::unique_ptr<ix::WebSocketServer> server;
    std::atomic_bool busy = false;
    std::string auth_token;
    bool network_initialized = false;

    Impl(AppConfig config_value, InferenceEngine & engine_value)
        : config(std::move(config_value)), engine(engine_value) {
        if (!ix::initNetSystem()) {
            throw std::runtime_error("failed to initialize the network subsystem");
        }
        network_initialized = true;
        if (config.auth.mode == AuthMode::automatic) auth_token = protocol::make_auth_token();
        if (config.auth.mode == AuthMode::fixed) auth_token = config.auth.token;
    }

    ~Impl() {
        if (server) server->stop();
        server.reset();
        if (network_initialized) ix::uninitNetSystem();
    }
};

WebSocketServer::WebSocketServer(AppConfig config, InferenceEngine & engine)
    : impl_(std::make_unique<Impl>(std::move(config), engine)) {}

WebSocketServer::~WebSocketServer() = default;

std::uint16_t WebSocketServer::start() {
    if (impl_->server) {
        throw std::logic_error("WebSocket server is already started");
    }
    int port = impl_->config.port;
    if (port == 0) {
        port = ix::getFreePort();
        if (port <= 0 || port > 65535) {
            throw std::runtime_error("failed to reserve an available TCP port");
        }
    }

    impl_->server = std::make_unique<ix::WebSocketServer>(
        port, impl_->config.host, ix::SocketServer::kDefaultTcpBacklog, 8);
    impl_->server->disablePerMessageDeflate();
    impl_->server->setOnConnectionCallback(
        [owner = impl_.get()](std::weak_ptr<ix::WebSocket> weak_socket, std::shared_ptr<ix::ConnectionState>) {
            const auto socket = weak_socket.lock();
            if (!socket) return;
            auto connection = std::make_shared<Impl::Connection>(*owner, weak_socket);
            socket->setOnMessageCallback(
                [connection = std::move(connection)](const ix::WebSocketMessagePtr & message) {
                    connection->on_message(message);
                });
        });

    const auto [listening, error] = impl_->server->listen();
    if (!listening) {
        impl_->server.reset();
        throw std::runtime_error(error);
    }
    impl_->server->start();
    return static_cast<std::uint16_t>(port);
}

void WebSocketServer::wait() {
    if (impl_->server) impl_->server->wait();
}

void WebSocketServer::stop() {
    if (impl_->server) impl_->server->stop();
}

const std::string & WebSocketServer::auth_token() const noexcept {
    return impl_->auth_token;
}

} // namespace r2t2
