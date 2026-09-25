#pragma once

#include "r2t2/config.h"
#include "r2t2/inference.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace r2t2 {

class WebSocketServer final {
public:
    WebSocketServer(AppConfig config, InferenceEngine & engine);
    ~WebSocketServer();

    std::uint16_t start();
    void wait();
    void stop();
    const std::string & auth_token() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace r2t2

