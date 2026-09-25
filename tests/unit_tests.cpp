#include "r2t2/config.h"
#include "r2t2/inference.h"
#include "r2t2/protocol.h"
#include "r2t2/session.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class FakeEngine final : public r2t2::InferenceEngine {
public:
    std::vector<std::string> outputs;
    std::vector<std::size_t> audio_sizes;
    std::vector<std::string> prompts;
    std::vector<std::uint32_t> token_limits;

    r2t2::Generation generate(
        std::span<const float> audio,
        std::string_view prompt,
        std::uint32_t max_tokens) override {
        audio_sizes.push_back(audio.size());
        prompts.emplace_back(prompt);
        token_limits.push_back(max_tokens);
        const auto index = audio_sizes.size() - 1;
        return {index < outputs.size() ? outputs[index] : std::string{}, {}, false};
    }

    std::vector<std::int32_t> tokenize(std::string_view text) const override {
        std::vector<std::int32_t> tokens;
        tokens.reserve(text.size());
        for (const auto value : text) {
            tokens.push_back(static_cast<unsigned char>(value));
        }
        return tokens;
    }

    std::string detokenize(std::span<const std::int32_t> tokens) const override {
        std::string text;
        text.reserve(tokens.size());
        for (const auto token : tokens) text.push_back(static_cast<char>(token));
        return text;
    }
};

std::vector<std::int16_t> silence(std::size_t samples) {
    return std::vector<std::int16_t>(samples, 0);
}

void test_pcm_parser() {
    const std::byte bytes[] = {
        std::byte{0x00}, std::byte{0x80},
        std::byte{0xff}, std::byte{0x7f},
        std::byte{0xff}, std::byte{0xff},
    };
    const auto samples = r2t2::protocol::parse_pcm_s16le(bytes);
    require(samples == std::vector<std::int16_t>({-32768, 32767, -1}), "little-endian PCM parsing failed");

    bool rejected = false;
    try {
        r2t2::protocol::parse_pcm_s16le(std::span<const std::byte>(bytes, 1));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "odd PCM byte count was not rejected");
}

void test_header() {
    r2t2::StreamingSettings defaults;
    const auto header = r2t2::protocol::parse_header(
        R"({"requestId":"test-id","channels":1,"sample_rate":16000,"language":"zhen","use_vad":false,"system_prompt":"names","mode":"fast","smooth":true})",
        defaults,
        r2t2::protocol::WireDialect::native);
    require(header.request_id == "test-id", "request ID was not preserved");
    require(header.language == "Auto", "zhen language alias was not mapped");
    require(header.context == "names", "system prompt was not preserved");
    require(header.mode == r2t2::RecognitionMode::fast, "fast recognition mode was not preserved");
    require(header.smooth, "native smooth option was not preserved");
}

void test_rstream_header() {
    r2t2::StreamingSettings defaults;
    const auto header = r2t2::protocol::parse_header(
        R"({"requestId":"rstream-id","lang":"cn","booked_words":"Technical words: Tiginal, R2T2","use_vad":false,"smooth":true})",
        defaults,
        r2t2::protocol::WireDialect::rstream);
    require(header.request_id == "rstream-id", "rstream request ID was not preserved");
    require(header.language == "Chinese", "rstream language code was not mapped");
    require(
        header.context == "Technical words: Tiginal, R2T2",
        "rstream booked words were not mapped to recognition context");
    require(header.smooth, "rstream smooth option was not preserved");

    const auto spanish = r2t2::protocol::parse_header(
        R"({"lang":"es"})",
        defaults,
        r2t2::protocol::WireDialect::rstream);
    require(spanish.language == "Spanish", "the canonical Spanish language code was not mapped");
}

void test_header_defaults_and_validation() {
    r2t2::StreamingSettings defaults;
    defaults.mode = r2t2::RecognitionMode::fast;
    defaults.smooth = true;
    const auto header = r2t2::protocol::parse_header(
        R"({})", defaults, r2t2::protocol::WireDialect::rstream);
    require(header.mode == r2t2::RecognitionMode::fast, "header did not inherit the CLI recognition mode");
    require(header.smooth, "header did not inherit the CLI smooth option");

    bool rejected = false;
    try {
        r2t2::protocol::parse_header(
            R"({"mode":"turbo"})", defaults, r2t2::protocol::WireDialect::native);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "an unsupported recognition mode was not rejected");
}

void test_wire_dialect_output() {
    const r2t2::RecognitionUpdate update{" world", true, 12.5, 18.75};
    const auto native = nlohmann::json::parse(r2t2::protocol::success_json(
        r2t2::protocol::WireDialect::native, "request-id", update, "Hello world"));
    require(native.at("msg").at("text") == " world", "native output stopped using text deltas");
    require(native.at("msg").at("reset") == true, "native final output stopped resetting the segment");

    const auto rstream = nlohmann::json::parse(r2t2::protocol::success_json(
        r2t2::protocol::WireDialect::rstream, "request-id", update, "Hello world"));
    require(rstream.at("msg").at("text") == "Hello world", "rstream output was not cumulative");
    require(rstream.at("is_final") == true, "rstream final output was not marked final");
    require(
        r2t2::protocol::is_eos_command("YOUDAO_ASR_EOS")
            && r2t2::protocol::is_eos_command("YOUDAO_ONETIME_ASR_STREAM_EOS"),
        "server did not accept both R2T2 EOS commands");
}

void test_chunking_and_delta_output() {
    FakeEngine engine;
    engine.outputs = {"Hello", "o world"};
    r2t2::StreamingSettings settings;
    r2t2::R2T2Session session(engine, settings);

    auto updates = session.push_pcm(silence(2559));
    require(updates.empty(), "a partial first chunk decoded too early");
    updates = session.push_pcm(silence(2561));
    require(updates.size() == 1, "the 320 ms first chunk did not decode exactly once");
    require(updates[0].text_delta == "Hell", "first stable delta was incorrect");
    require(engine.audio_sizes[0] == 5120, "first inference audio size was incorrect");
    require(engine.token_limits[0] == 4, "first inference token budget was incorrect");

    updates = session.push_pcm(silence(2560));
    require(updates.size() == 1, "the steady 160 ms chunk did not decode exactly once");
    require(updates[0].text_delta == "o worl", "second response was not delta-only");
    require(engine.audio_sizes[1] == 7680, "streaming audio accumulation was incorrect");
    require(engine.token_limits[1] == 2, "steady inference token budget was incorrect");
}

void test_tail_flush() {
    FakeEngine engine;
    engine.outputs = {"Hello", "o"};
    r2t2::StreamingSettings settings;
    r2t2::R2T2Session session(engine, settings);

    const auto updates = session.push_pcm(silence(5120));
    require(updates.size() == 1 && updates[0].text_delta == "Hell", "tail setup failed");
    require(session.push_pcm(silence(137)).empty(), "tail decoded before EOS");
    const auto final = session.finish();
    require(final.final, "EOS update was not marked final");
    require(final.text_delta == "o", "EOS discarded the incomplete audio tail");
    require(engine.audio_sizes.back() == 5257, "EOS inference did not include the tail samples");
}

void test_exact_boundary_eos() {
    FakeEngine engine;
    engine.outputs = {"Hello"};
    r2t2::R2T2Session session(engine, {});
    session.push_pcm(silence(5120));
    const auto final = session.finish();
    require(final.final && final.text_delta.empty(), "exact-boundary EOS did not match upstream behavior");
    require(engine.audio_sizes.size() == 1, "exact-boundary EOS issued an extra inference request");
}

void test_auto_language_gate() {
    FakeEngine engine;
    engine.outputs = {"unlabeled text"};
    r2t2::StreamingSettings settings;
    settings.language = "Auto";
    settings.unfixed_tokens = 0;
    r2t2::R2T2Session session(engine, settings);
    const auto updates = session.push_pcm(silence(5120));
    require(updates.size() == 1 && updates[0].text_delta.empty(), "auto language emitted text before the ASR tag");
}

void test_prefix_revision_is_ignored() {
    FakeEngine engine;
    engine.outputs = {"Hello", "x"};
    r2t2::R2T2Session session(engine, {});
    const auto first = session.push_pcm(silence(5120));
    const auto second = session.push_pcm(silence(2560));
    require(first.size() == 1 && first[0].text_delta == "Hell", "revision setup failed");
    require(second.size() == 1 && second[0].text_delta.empty(), "a model prefix revision escaped as stable text");
}

void test_recognition_mode_and_smooth_prompt() {
    FakeEngine slow_engine;
    slow_engine.outputs = {"Hello."};
    r2t2::R2T2Session slow_session(slow_engine, {});
    const auto slow = slow_session.push_pcm(silence(5120));
    require(slow.size() == 1 && slow[0].text_delta == "Hello", "slow mode did not retain one rollback token");

    FakeEngine fast_engine;
    fast_engine.outputs = {"Hello."};
    r2t2::StreamingSettings settings;
    settings.mode = r2t2::RecognitionMode::fast;
    settings.smooth = true;
    settings.context = "Names: Tiginal";
    r2t2::R2T2Session fast_session(fast_engine, settings);
    const auto fast = fast_session.push_pcm(silence(5120));
    require(fast.size() == 1 && fast[0].text_delta == "Hello.", "fast mode did not commit punctuation");
    require(
        fast_engine.prompts[0].find("system\nSmooth the text\nNames: Tiginal<|im_end|>")
            != std::string::npos,
        "smooth mode did not add the upstream prompt before the session context");
}

void test_rolling_window() {
    FakeEngine engine;
    r2t2::StreamingSettings settings;
    settings.unfixed_tokens = 0;
    r2t2::R2T2Session session(engine, settings);

    session.push_pcm(silence(5120 + 99 * 2560));
    require(engine.audio_sizes.size() == 100, "unexpected number of rolling-window decodes");
    require(engine.audio_sizes[98] == 256000, "audio did not reach the 16 second upstream window");
    require(engine.audio_sizes[99] == 130560, "audio and text were not rolled forward by 8 seconds");
}

void test_probe_arguments_without_models() {
    char program[] = "r2t2-ws";
    char probe[] = "--probe";
    char * argv[] = {program, probe};
    const auto config = r2t2::parse_arguments(2, argv);
    require(config.mode == r2t2::RunMode::probe, "probe mode was not parsed");
    require(!config.models, "probe unexpectedly required model files");
}

void test_debug_arguments() {
    char program[] = "r2t2-ws";
    char probe[] = "--probe";
    char debug[] = "--debug";
    char * argv[] = {program, probe, debug};
    const auto config = r2t2::parse_arguments(3, argv);
    require(config.debug, "debug mode was not parsed");
    require(config.verbose, "debug mode did not enable diagnostic logging");
}

void test_recognition_arguments() {
    char program[] = "r2t2-ws";
    char probe[] = "--probe";
    char mode[] = "--mode";
    char fast[] = "fast";
    char smooth[] = "--smooth";
    char * argv[] = {program, probe, mode, fast, smooth};
    const auto config = r2t2::parse_arguments(5, argv);
    require(config.streaming.mode == r2t2::RecognitionMode::fast, "--mode was not parsed");
    require(config.streaming.smooth, "--smooth was not parsed");
}

void test_websocket_url() {
    const auto url = r2t2::protocol::websocket_url(
        "127.0.0.1", 8272, "/asr_stream_api_v1", "secret value&x");
    require(
        url == "ws://127.0.0.1:8272/asr_stream_api_v1?token=secret%20value%26x",
        "authenticated WebSocket URL was incorrect");

    const auto ready = r2t2::protocol::ready_json(
        "127.0.0.1", 8272, "/asr_stream_api_v1", "secret");
    const auto expected = R"({
  "host": "127.0.0.1",
  "port": 8272,
  "endpoint": "/asr_stream_api_v1",
  "url": "ws://127.0.0.1:8272/asr_stream_api_v1?token=secret",
  "rstreamEndpoint": "/asr",
  "rstreamUrl": "ws://127.0.0.1:8272/asr?t=secret",
  "authToken": "secret",
  "event": "ready"
})";
    require(ready == expected, "readiness JSON did not match the documented format");

    const auto unauthenticated = r2t2::protocol::ready_json(
        "127.0.0.1", 8272, "/asr_stream_api_v1", "");
    const auto expected_unauthenticated = R"({
  "host": "127.0.0.1",
  "port": 8272,
  "endpoint": "/asr_stream_api_v1",
  "url": "ws://127.0.0.1:8272/asr_stream_api_v1",
  "rstreamEndpoint": "/asr",
  "rstreamUrl": "ws://127.0.0.1:8272/asr",
  "event": "ready"
})";
    require(
        unauthenticated == expected_unauthenticated,
        "unauthenticated readiness JSON did not match the documented format");
}

} // namespace

int main() {
    try {
        test_pcm_parser();
        test_header();
        test_rstream_header();
        test_header_defaults_and_validation();
        test_wire_dialect_output();
        test_chunking_and_delta_output();
        test_tail_flush();
        test_exact_boundary_eos();
        test_auto_language_gate();
        test_prefix_revision_is_ignored();
        test_recognition_mode_and_smooth_prompt();
        test_rolling_window();
        test_probe_arguments_without_models();
        test_debug_arguments();
        test_recognition_arguments();
        test_websocket_url();
        std::cout << "all unit tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception & error) {
        std::cerr << "unit test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
