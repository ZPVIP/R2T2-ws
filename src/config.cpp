#include "r2t2/config.h"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace r2t2 {
namespace {

template <typename Integer>
Integer parse_integer(const std::string & name, const std::string & value) {
    Integer result{};
    const auto * begin = value.data();
    const auto * end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::invalid_argument(name + " expects an integer, got: " + value);
    }
    return result;
}

std::string take_value(int & index, int argc, char ** argv, const std::string & option) {
    if (++index >= argc) {
        throw std::invalid_argument(option + " requires a value");
    }
    return argv[index];
}

void validate_streaming(const StreamingSettings & settings) {
    if (settings.chunk_ms < 80 || settings.chunk_ms > 2000) {
        throw std::invalid_argument("--chunk-ms must be between 80 and 2000");
    }
    if (settings.lookahead_ms > 2000) {
        throw std::invalid_argument("--lookahead-ms must be between 0 and 2000");
    }
    if (settings.unfixed_tokens > 64) {
        throw std::invalid_argument("--unfixed-token-num must not exceed 64");
    }
    if (settings.language.empty()) {
        throw std::invalid_argument("--language must not be empty; use Auto for language detection");
    }
}

} // namespace

RecognitionMode parse_recognition_mode(std::string_view value) {
    if (value == "slow") return RecognitionMode::slow;
    if (value == "fast") return RecognitionMode::fast;
    throw std::invalid_argument("recognition mode must be slow or fast, got: " + std::string(value));
}

std::string_view recognition_mode_name(RecognitionMode mode) noexcept {
    return mode == RecognitionMode::fast ? "fast" : "slow";
}

ModelPaths discover_models(const std::filesystem::path & directory) {
    if (!std::filesystem::is_directory(directory)) {
        throw std::invalid_argument("GGUF directory does not exist: " + directory.string());
    }

    std::vector<std::filesystem::path> models;
    std::vector<std::filesystem::path> projectors;
    for (const auto & entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".gguf") {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.rfind("mmproj", 0) == 0) {
            projectors.push_back(entry.path());
        } else {
            models.push_back(entry.path());
        }
    }
    std::sort(models.begin(), models.end());
    std::sort(projectors.begin(), projectors.end());

    if (models.size() != 1 || projectors.size() != 1) {
        std::ostringstream message;
        message << "--gguf-dir requires exactly one model GGUF and one mmproj*.gguf; found models=[";
        for (std::size_t i = 0; i < models.size(); ++i) {
            message << (i ? ", " : "") << models[i].filename().string();
        }
        message << "], projectors=[";
        for (std::size_t i = 0; i < projectors.size(); ++i) {
            message << (i ? ", " : "") << projectors[i].filename().string();
        }
        message << ']';
        throw std::invalid_argument(message.str());
    }
    return {models.front(), projectors.front()};
}

AppConfig parse_arguments(int argc, char ** argv) {
    AppConfig config;
    config.inference.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::filesystem::path model;
    std::filesystem::path mmproj;
    std::filesystem::path gguf_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") {
            config.mode = RunMode::help;
        } else if (option == "--version") {
            config.mode = RunMode::version;
        } else if (option == "--probe") {
            config.mode = RunMode::probe;
        } else if (option == "--model") {
            model = take_value(i, argc, argv, option);
        } else if (option == "--mmproj") {
            mmproj = take_value(i, argc, argv, option);
        } else if (option == "--gguf-dir") {
            gguf_dir = take_value(i, argc, argv, option);
        } else if (option == "--host") {
            config.host = take_value(i, argc, argv, option);
        } else if (option == "--port") {
            const auto port = parse_integer<unsigned int>(option, take_value(i, argc, argv, option));
            if (port > 65535) {
                throw std::invalid_argument("--port must be between 0 and 65535");
            }
            config.port = static_cast<std::uint16_t>(port);
        } else if (option == "--language") {
            config.streaming.language = take_value(i, argc, argv, option);
        } else if (option == "--chunk-ms") {
            config.streaming.chunk_ms = parse_integer<std::uint32_t>(option, take_value(i, argc, argv, option));
        } else if (option == "--lookahead-ms") {
            config.streaming.lookahead_ms = parse_integer<std::uint32_t>(option, take_value(i, argc, argv, option));
        } else if (option == "--unfixed-token-num") {
            config.streaming.unfixed_tokens = parse_integer<std::uint32_t>(option, take_value(i, argc, argv, option));
        } else if (option == "--context") {
            config.streaming.context = take_value(i, argc, argv, option);
        } else if (option == "--mode") {
            config.streaming.mode = parse_recognition_mode(take_value(i, argc, argv, option));
        } else if (option == "--smooth") {
            config.streaming.smooth = true;
        } else if (option == "--threads") {
            config.inference.threads = parse_integer<int>(option, take_value(i, argc, argv, option));
        } else if (option == "--gpu-layers") {
            config.inference.gpu_layers = parse_integer<int>(option, take_value(i, argc, argv, option));
        } else if (option == "--auth") {
            const auto value = take_value(i, argc, argv, option);
            if (value == "off") {
                config.auth = {AuthMode::off, {}};
            } else if (value == "auto") {
                config.auth = {AuthMode::automatic, {}};
            } else if (!value.empty()) {
                config.auth = {AuthMode::fixed, value};
            } else {
                throw std::invalid_argument("--auth expects off, auto, or a fixed token");
            }
        } else if (option == "--verbose") {
            config.verbose = true;
        } else if (option == "--debug") {
            config.debug = true;
            config.verbose = true;
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }

    if (!gguf_dir.empty() && (!model.empty() || !mmproj.empty())) {
        throw std::invalid_argument("use either --gguf-dir or --model with --mmproj, not both");
    }
    if (!gguf_dir.empty()) {
        config.models = discover_models(gguf_dir);
    } else if (!model.empty() || !mmproj.empty()) {
        if (model.empty() || mmproj.empty()) {
            throw std::invalid_argument("--model and --mmproj must be provided together");
        }
        config.models = ModelPaths{model, mmproj};
    }

    validate_streaming(config.streaming);
    if (config.inference.threads <= 0) {
        throw std::invalid_argument("--threads must be positive");
    }
    if (config.inference.gpu_layers < -1) {
        throw std::invalid_argument("--gpu-layers must be -1 or greater");
    }
    if (config.mode == RunMode::server && !config.models) {
        throw std::invalid_argument("server mode requires --model with --mmproj, or --gguf-dir");
    }
    if (config.models) {
        if (!std::filesystem::is_regular_file(config.models->model)) {
            throw std::invalid_argument("model file does not exist: " + config.models->model.string());
        }
        if (!std::filesystem::is_regular_file(config.models->mmproj)) {
            throw std::invalid_argument("mmproj file does not exist: " + config.models->mmproj.string());
        }
    }
    return config;
}

std::string help_text() {
    return R"(r2t2-ws 0.1.0

Usage:
  r2t2-ws --model <model.gguf> --mmproj <mmproj.gguf> [options]
  r2t2-ws --gguf-dir <directory> [options]
  r2t2-ws --probe

Options:
  --model <path>             Main Confucius4-R2T2 GGUF
  --mmproj <path>            Compatible mmproj GGUF
  --gguf-dir <path>          Directory containing exactly one of each GGUF
  --host <address>           Bind address (default: 127.0.0.1)
  --port <number>            Bind port, 0 asks the OS for a free port (default: 8272)
  --language <name>          Forced language or Auto (default: Chinese)
  --chunk-ms <number>        Decode chunk from 80 to 2000 ms (default: 160)
  --lookahead-ms <number>    Extra first-chunk audio (default: 160)
  --unfixed-token-num <n>    Trailing rollback token count (default: 1)
  --context <text>           Optional context or hot words
  --mode <slow|fast>         Stable-text commit mode (default: slow)
  --smooth                   Add the upstream smooth-text prompt
  --threads <number>         CPU worker threads
  --gpu-layers <number>      llama.cpp GPU layers, -1 means all (default: -1)
  --auth <off|auto|token>    Local WebSocket authentication (default: off)
  --verbose                  Enable diagnostic logs on stderr
  --debug                    Trace WebSocket input and output on stderr
  --probe                    Print runtime JSON and exit
  --version                  Print version and exit
  --help                     Print this help and exit
)";
}

} // namespace r2t2
