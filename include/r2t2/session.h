#pragma once

#include "r2t2/config.h"
#include "r2t2/inference.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace r2t2 {

struct RecognitionUpdate {
    std::string text_delta;
    bool final = false;
    double asr_cost_ms = 0.0;
    double total_cost_ms = 0.0;
};

class R2T2Session final {
public:
    R2T2Session(InferenceEngine & engine, StreamingSettings settings);

    std::vector<RecognitionUpdate> push_pcm(std::span<const std::int16_t> samples);
    RecognitionUpdate finish();
    bool has_audio() const noexcept;
    bool finished() const noexcept;

private:
    struct AudioSlice {
        std::size_t samples = 0;
        std::string fixed_delta;
    };

    RecognitionUpdate decode(std::span<const float> new_audio, bool final);
    void append_audio(std::span<const float> audio);
    void trim_window();
    std::string window_prefix() const;
    std::string build_base_prompt() const;
    std::string normalize_generation(std::string value) const;
    std::string fixed_prefix(std::string_view raw) const;
    std::pair<std::string, std::string> parse_output(std::string_view raw) const;

    InferenceEngine & engine_;
    StreamingSettings settings_;
    std::vector<float> pending_;
    std::size_t pending_begin_ = 0;
    std::vector<float> audio_window_;
    std::size_t audio_begin_ = 0;
    std::deque<AudioSlice> slices_;
    std::string detected_language_;
    std::string raw_decoded_;
    std::string base_prompt_;
    double max_tokens_ = 1.0;
    std::uint32_t max_tokens_ceiling_ = 4;
    bool last_emitted_token_chinese_ = false;
    bool first_chunk_ = true;
    bool saw_audio_ = false;
    bool finished_ = false;
};

} // namespace r2t2
