#pragma once

#include "r2t2/session.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace r2t2::protocol {

inline constexpr std::string_view native_eos_command = "YOUDAO_ONETIME_ASR_STREAM_EOS";
inline constexpr std::string_view rstream_eos_command = "YOUDAO_ASR_EOS";
inline constexpr std::string_view rstream_endpoint = "/asr";

enum class WireDialect { native, rstream };

struct ClientHeader {
    std::string request_id;
    std::string language;
    std::string context;
    RecognitionMode mode = RecognitionMode::slow;
    bool use_vad = false;
    bool smooth = false;
};

ClientHeader parse_header(
    std::string_view text,
    const StreamingSettings & defaults,
    WireDialect dialect);
bool is_eos_command(std::string_view text) noexcept;
std::vector<std::int16_t> parse_pcm_s16le(std::span<const std::byte> bytes);
std::string make_request_id();
std::string make_auth_token();
std::string success_json(
    WireDialect dialect,
    std::string_view request_id,
    const RecognitionUpdate & update,
    std::string_view committed_text);
std::string error_json(
    std::string_view request_id,
    std::string_view code,
    std::string_view message);
std::string connected_json(std::string_view request_id);
std::string probe_json(const RuntimeInfo & runtime);
std::string websocket_url(
    std::string_view host,
    std::uint16_t port,
    std::string_view endpoint,
    std::string_view auth_token,
    std::string_view auth_parameter = "token");
std::string ready_json(
    std::string_view host,
    std::uint16_t port,
    std::string_view endpoint,
    std::string_view auth_token);

} // namespace r2t2::protocol
