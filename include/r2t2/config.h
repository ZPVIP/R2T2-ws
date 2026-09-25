#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace r2t2 {

struct ModelPaths {
    std::filesystem::path model;
    std::filesystem::path mmproj;
};

struct InferenceOptions {
    std::uint32_t context_tokens = 32768;
    std::uint32_t batch_tokens = 8192;
    int threads = 0;
    int gpu_layers = -1;
    bool use_gpu = true;
};

enum class RecognitionMode { slow, fast };

struct StreamingSettings {
    std::string language = "Chinese";
    std::string context;
    RecognitionMode mode = RecognitionMode::slow;
    bool smooth = false;
    std::uint32_t chunk_ms = 160;
    std::uint32_t lookahead_ms = 160;
    std::uint32_t unfixed_tokens = 1;
    std::uint32_t max_window_ms = 16000;
    std::uint32_t discard_window_ms = 8000;
};

enum class AuthMode { off, automatic, fixed };

struct AuthSettings {
    AuthMode mode = AuthMode::off;
    std::string token;
};

enum class RunMode { server, probe, version, help };

struct AppConfig {
    RunMode mode = RunMode::server;
    std::optional<ModelPaths> models;
    InferenceOptions inference;
    StreamingSettings streaming;
    AuthSettings auth;
    std::string host = "127.0.0.1";
    std::uint16_t port = 8272;
    std::string endpoint = "/asr_stream_api_v1";
    bool verbose = false;
    bool debug = false;
};

AppConfig parse_arguments(int argc, char ** argv);
ModelPaths discover_models(const std::filesystem::path & directory);
RecognitionMode parse_recognition_mode(std::string_view value);
std::string_view recognition_mode_name(RecognitionMode mode) noexcept;
std::string help_text();

} // namespace r2t2
