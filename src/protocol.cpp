#include "r2t2/protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

#if defined(__APPLE__)
#include <Security/Security.h>
#elif defined(_WIN32)
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#endif

namespace r2t2::protocol {
namespace {

void secure_random(std::span<std::uint8_t> output) {
#if defined(__APPLE__)
    if (SecRandomCopyBytes(kSecRandomDefault, output.size(), output.data()) == errSecSuccess) {
        return;
    }
#elif defined(_WIN32)
    if (BCryptGenRandom(nullptr, output.data(), static_cast<ULONG>(output.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
        return;
    }
#elif defined(__linux__)
    if (getrandom(output.data(), output.size(), 0) == static_cast<ssize_t>(output.size())) {
        return;
    }
#endif
    std::random_device random;
    for (auto & byte : output) {
        byte = static_cast<std::uint8_t>(random());
    }
}

std::string hex(std::span<const std::uint8_t> bytes) {
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        result << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return result.str();
}

std::string rstream_language(std::string_view language) {
    static constexpr std::array mappings{
        std::pair{"ar", "Arabic"},
        std::pair{"yue", "Cantonese"},
        std::pair{"cn", "Chinese"},
        std::pair{"cs", "Czech"},
        std::pair{"da", "Danish"},
        std::pair{"de", "German"},
        std::pair{"nl", "Dutch"},
        std::pair{"el", "Greek"},
        std::pair{"en", "English"},
        std::pair{"es", "Spanish"},
        std::pair{"sp", "Spanish"},
        std::pair{"fa", "Persian"},
        std::pair{"fil", "Filipino"},
        std::pair{"fi", "Finnish"},
        std::pair{"fr", "French"},
        std::pair{"hi", "Hindi"},
        std::pair{"hu", "Hungarian"},
        std::pair{"id", "Indonesian"},
        std::pair{"it", "Italian"},
        std::pair{"jp", "Japanese"},
        std::pair{"ko", "Korean"},
        std::pair{"mk", "Macedonian"},
        std::pair{"ms", "Malay"},
        std::pair{"pl", "Polish"},
        std::pair{"pt", "Portuguese"},
        std::pair{"ro", "Romanian"},
        std::pair{"ru", "Russian"},
        std::pair{"sv", "Swedish"},
        std::pair{"th", "Thai"},
        std::pair{"tr", "Turkish"},
        std::pair{"vi", "Vietnamese"},
    };
    const auto match = std::find_if(mappings.begin(), mappings.end(), [language](const auto & mapping) {
        return mapping.first == language;
    });
    return match == mappings.end() ? std::string(language) : std::string(match->second);
}

} // namespace

ClientHeader parse_header(
    std::string_view text,
    const StreamingSettings & defaults,
    WireDialect dialect) {
    const auto json = nlohmann::json::parse(text);
    if (!json.is_object()) {
        throw std::invalid_argument("initial text message must be a JSON object");
    }

    ClientHeader header;
    header.request_id = json.value("requestId", make_request_id());
    header.language = dialect == WireDialect::rstream
        ? rstream_language(json.value("lang", defaults.language))
        : json.value("language", defaults.language);
    header.mode = parse_recognition_mode(
        json.value("mode", std::string(recognition_mode_name(defaults.mode))));
    header.use_vad = json.value("use_vad", false);
    if (json.contains("smooth") && !json.at("smooth").is_boolean()) {
        throw std::invalid_argument("smooth must be a boolean");
    }
    header.smooth = json.value("smooth", defaults.smooth);
    if (dialect == WireDialect::native
        && json.contains("channels") && json.at("channels").get<int>() != 1) {
        throw std::invalid_argument("channels must be 1");
    }
    if (dialect == WireDialect::native
        && json.contains("sample_rate") && json.at("sample_rate").get<int>() != 16000) {
        throw std::invalid_argument("sample_rate must be 16000");
    }
    if (header.use_vad) {
        throw std::invalid_argument("VAD is not available; send use_vad=false and use EOS");
    }
    if (header.language == "zhen") {
        header.language = "Auto";
    }
    const auto context_key = dialect == WireDialect::rstream ? "booked_words" : "system_prompt";
    if (json.contains(context_key)) {
        header.context = json.at(context_key).get<std::string>();
        if (header.context.size() > 4000) {
            throw std::invalid_argument(std::string(context_key) + " must not exceed 4000 bytes");
        }
    } else {
        header.context = defaults.context;
    }
    return header;
}

bool is_eos_command(std::string_view text) noexcept {
    return text == native_eos_command || text == rstream_eos_command;
}

std::vector<std::int16_t> parse_pcm_s16le(std::span<const std::byte> bytes) {
    if (bytes.size() % 2 != 0) {
        throw std::invalid_argument("PCM binary message must contain an even number of bytes");
    }
    std::vector<std::int16_t> samples(bytes.size() / 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto low = std::to_integer<std::uint8_t>(bytes[i * 2]);
        const auto high = std::to_integer<std::uint8_t>(bytes[i * 2 + 1]);
        samples[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(low) |
                                               (static_cast<std::uint16_t>(high) << 8));
    }
    return samples;
}

std::string make_request_id() {
    std::array<std::uint8_t, 16> bytes{};
    secure_random(bytes);
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    const auto value = hex(bytes);
    return value.substr(0, 8) + '-' + value.substr(8, 4) + '-' + value.substr(12, 4) + '-' +
           value.substr(16, 4) + '-' + value.substr(20);
}

std::string make_auth_token() {
    std::array<std::uint8_t, 32> bytes{};
    secure_random(bytes);
    return hex(bytes);
}

std::string success_json(
    WireDialect dialect,
    std::string_view request_id,
    const RecognitionUpdate & update,
    std::string_view committed_text) {
    if (dialect == WireDialect::rstream) {
        return nlohmann::json{
            {"status", "success"},
            {"requestId", request_id},
            {"is_final", update.final},
            {"msg", {
                {"text", committed_text},
                {"asr_cost_ms", update.asr_cost_ms},
                {"total_cost_ms", update.total_cost_ms},
            }},
        }.dump();
    }
    return nlohmann::json{
        {"status", "success"},
        {"requestId", request_id},
        {"msg", {
            {"text", update.text_delta},
            {"reset", update.final},
            {"asr_cost_ms", update.asr_cost_ms},
            {"total_cost_ms", update.total_cost_ms},
        }},
    }.dump();
}

std::string error_json(
    std::string_view request_id,
    std::string_view code,
    std::string_view message) {
    return nlohmann::json{
        {"status", "error"},
        {"requestId", request_id},
        {"error", {{"code", code}, {"message", message}}},
    }.dump();
}

std::string connected_json(std::string_view request_id) {
    return nlohmann::json{
        {"status", "connected"},
        {"requestId", request_id},
        {"msg", ""},
        {"active_connections", 1},
    }.dump();
}

std::string probe_json(const RuntimeInfo & runtime) {
    return nlohmann::json{
        {"name", "r2t2-ws"},
        {"version", R2T2_VERSION},
        {"llama_cpp_commit", R2T2_LLAMA_CPP_COMMIT},
        {"backend", runtime.backend},
        {"devices", runtime.devices},
        {"r2t2", true},
        {"sample_rate", 16000},
        {"sample_format", "s16le"},
        {"channels", 1},
    }.dump();
}

std::string websocket_url(
    std::string_view host,
    std::uint16_t port,
    std::string_view endpoint,
    std::string_view auth_token,
    std::string_view auth_parameter) {
    std::ostringstream url;
    url << "ws://";
    if (host.find(':') != std::string_view::npos && !host.starts_with('[')) {
        url << '[' << host << ']';
    } else {
        url << host;
    }
    url << ':' << port << endpoint;
    if (!auth_token.empty()) {
        url << '?' << auth_parameter << '=' << std::uppercase << std::hex << std::setfill('0');
        for (const auto value : auth_token) {
            const auto byte = static_cast<unsigned char>(value);
            if ((byte >= 'A' && byte <= 'Z')
                || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9')
                || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
                url << static_cast<char>(byte);
            } else {
                url << '%' << std::setw(2) << static_cast<unsigned int>(byte);
            }
        }
    }
    return url.str();
}

std::string ready_json(
    std::string_view host,
    std::uint16_t port,
    std::string_view endpoint,
    std::string_view auth_token) {
    nlohmann::ordered_json json{
        {"host", host},
        {"port", port},
        {"endpoint", endpoint},
        {"url", websocket_url(host, port, endpoint, auth_token)},
        {"rstreamEndpoint", rstream_endpoint},
        {"rstreamUrl", websocket_url(host, port, rstream_endpoint, auth_token, "t")},
    };
    if (!auth_token.empty()) {
        json["authToken"] = auth_token;
    }
    json["event"] = "ready";
    return json.dump(2);
}

} // namespace r2t2::protocol
