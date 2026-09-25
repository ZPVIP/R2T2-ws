// Portions of the streaming policy are adapted from Confucius4-R2T2.
// Copyright 2026 The NetEase Youdao team.
// SPDX-License-Identifier: Apache-2.0

#include "r2t2/session.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace r2t2 {
namespace {

constexpr std::size_t sample_rate = 16000;
constexpr std::string_view asr_tag = "<asr_text>";
constexpr std::string_view replacement = "\xef\xbf\xbd";

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::uint32_t decode_utf8(std::string_view text, std::size_t & offset) {
    const auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80) {
        return first;
    }
    int continuation = 0;
    std::uint32_t codepoint = 0;
    if ((first & 0xe0U) == 0xc0U) {
        continuation = 1;
        codepoint = first & 0x1fU;
    } else if ((first & 0xf0U) == 0xe0U) {
        continuation = 2;
        codepoint = first & 0x0fU;
    } else if ((first & 0xf8U) == 0xf0U) {
        continuation = 3;
        codepoint = first & 0x07U;
    } else {
        return 0xfffdU;
    }
    while (continuation-- > 0) {
        if (offset >= text.size()) {
            return 0xfffdU;
        }
        const auto next = static_cast<unsigned char>(text[offset++]);
        if ((next & 0xc0U) != 0x80U) {
            return 0xfffdU;
        }
        codepoint = (codepoint << 6U) | (next & 0x3fU);
    }
    return codepoint;
}

bool is_chinese(std::uint32_t codepoint) {
    return codepoint >= 0x4e00U && codepoint <= 0x9fffU;
}

std::vector<std::pair<std::uint32_t, std::string>> utf8_parts(std::string_view text) {
    std::vector<std::pair<std::uint32_t, std::string>> parts;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto begin = offset;
        const auto codepoint = decode_utf8(text, offset);
        parts.emplace_back(codepoint, std::string(text.substr(begin, offset - begin)));
    }
    return parts;
}

bool contains_chinese(std::string_view text) {
    const auto parts = utf8_parts(text);
    return std::any_of(parts.begin(), parts.end(), [](const auto & part) {
        return is_chinese(part.first);
    });
}

bool ends_with_commit_punctuation(std::string_view text) {
    const auto parts = utf8_parts(trim(text));
    if (parts.empty()) return false;
    const auto codepoint = parts.back().first;
    return codepoint == ',' || codepoint == '.' || codepoint == '!'
        || codepoint == '?' || codepoint == ';' || codepoint == ':'
        || codepoint == 0xff0cU || codepoint == 0x3002U
        || codepoint == 0xff01U || codepoint == 0xff1fU
        || codepoint == 0x3001U || codepoint == 0xff1bU
        || codepoint == 0xff1aU;
}

std::string normalize_chinese_spacing(std::string_view text) {
    const auto parts = utf8_parts(text);
    std::string output;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto codepoint = parts[i].first;
        const bool ascii_space = codepoint == ' ' || codepoint == '\t' || codepoint == '\r' || codepoint == '\n';
        if (ascii_space && i > 0 && i + 1 < parts.size() &&
            is_chinese(parts[i - 1].first) && is_chinese(parts[i + 1].first)) {
            continue;
        }
        output += parts[i].second;
    }
    return output;
}

std::string normalize_punctuation(std::string_view text) {
    const auto parts = utf8_parts(text);
    std::string output;
    std::uint32_t previous = 0;
    for (const auto & [codepoint, bytes] : parts) {
        std::string replacement_text = bytes;
        if (is_chinese(previous)) {
            if (bytes == ",") replacement_text = "，";
            else if (bytes == ".") replacement_text = "。";
            else if (bytes == "!") replacement_text = "！";
            else if (bytes == "?") replacement_text = "？";
            else if (bytes == ";") replacement_text = "；";
            else if (bytes == ":") replacement_text = "：";
            else if (bytes == "(") replacement_text = "（";
            else if (bytes == ")") replacement_text = "）";
        } else if ((previous >= '0' && previous <= '9') ||
                   (previous >= 'A' && previous <= 'Z') ||
                   (previous >= 'a' && previous <= 'z') || previous == '\'' || previous == '"') {
            if (bytes == "，") replacement_text = ",";
            else if (bytes == "。") replacement_text = ".";
            else if (bytes == "！") replacement_text = "!";
            else if (bytes == "？") replacement_text = "?";
            else if (bytes == "；") replacement_text = ";";
            else if (bytes == "：") replacement_text = ":";
            else if (bytes == "（") replacement_text = "(";
            else if (bytes == "）") replacement_text = ")";
        }
        output += replacement_text;
        if (codepoint != ' ' && codepoint != '\t' && codepoint != '\r' && codepoint != '\n') {
            previous = codepoint;
        }
    }
    return output;
}

} // namespace

R2T2Session::R2T2Session(InferenceEngine & engine, StreamingSettings settings)
    : engine_(engine), settings_(std::move(settings)), base_prompt_(build_base_prompt()) {
    const auto chunk_samples = static_cast<std::size_t>(settings_.chunk_ms) * sample_rate / 1000;
    const auto first_samples = static_cast<std::size_t>(settings_.chunk_ms + settings_.lookahead_ms) * sample_rate / 1000;
    if (chunk_samples == 0 || first_samples == 0) {
        throw std::invalid_argument("streaming chunk configuration produced zero samples");
    }
    pending_.reserve(first_samples + chunk_samples);
    audio_window_.reserve(static_cast<std::size_t>(settings_.max_window_ms + settings_.chunk_ms) * sample_rate / 1000);
    max_tokens_ = std::max(1.0, static_cast<double>(first_samples) / 1280.0);
    max_tokens_ceiling_ = std::min<std::uint32_t>(
        32, std::max<std::uint32_t>(4, 2 * static_cast<std::uint32_t>(chunk_samples / 1280)));
}

std::vector<RecognitionUpdate> R2T2Session::push_pcm(std::span<const std::int16_t> samples) {
    if (finished_) {
        throw std::logic_error("cannot push audio after EOS");
    }
    pending_.reserve(pending_.size() + samples.size());
    for (const auto sample : samples) {
        pending_.push_back(static_cast<float>(sample) / 32768.0f);
    }
    if (!samples.empty()) {
        saw_audio_ = true;
    }

    std::vector<RecognitionUpdate> updates;
    const auto steady = static_cast<std::size_t>(settings_.chunk_ms) * sample_rate / 1000;
    const auto initial = static_cast<std::size_t>(settings_.chunk_ms + settings_.lookahead_ms) * sample_rate / 1000;
    while (pending_.size() - pending_begin_ >= (first_chunk_ ? initial : steady)) {
        const auto count = first_chunk_ ? initial : steady;
        const auto chunk = std::span<const float>(pending_.data() + pending_begin_, count);
        updates.push_back(decode(chunk, false));
        pending_begin_ += count;
        first_chunk_ = false;
    }
    if (pending_begin_ > 0 && (pending_begin_ >= pending_.size() / 2 || pending_begin_ > sample_rate)) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pending_begin_));
        pending_begin_ = 0;
    }
    return updates;
}

RecognitionUpdate R2T2Session::finish() {
    if (finished_) {
        return {{}, true, 0.0, 0.0};
    }
    finished_ = true;
    const auto remaining = pending_.size() - pending_begin_;
    if (!saw_audio_ || remaining == 0) {
        return {{}, true, 0.0, 0.0};
    }
    const auto tail = std::span<const float>(pending_.data() + pending_begin_, remaining);
    pending_begin_ = pending_.size();
    return decode(tail, true);
}

bool R2T2Session::has_audio() const noexcept {
    return saw_audio_;
}

bool R2T2Session::finished() const noexcept {
    return finished_;
}

RecognitionUpdate R2T2Session::decode(std::span<const float> new_audio, bool final) {
    const auto total_started = std::chrono::steady_clock::now();
    append_audio(new_audio);
    trim_window();

    const auto committed = window_prefix();
    std::string inference_prefix = committed;
    const bool automatic_language = settings_.language == "Auto" || settings_.language == "None";
    if (automatic_language && !detected_language_.empty()) {
        inference_prefix = "language " + detected_language_ + std::string(asr_tag) + committed;
    }

    const auto first_tokens = std::max<std::uint32_t>(
        1, ((settings_.chunk_ms + settings_.lookahead_ms) * static_cast<std::uint32_t>(sample_rate) / 1000) / 1280);
    const auto max_tokens = final
        ? first_tokens
        : std::max<std::uint32_t>(1, static_cast<std::uint32_t>(max_tokens_));

    const auto audio = std::span<const float>(
        audio_window_.data() + static_cast<std::ptrdiff_t>(audio_begin_),
        audio_window_.size() - audio_begin_);
    const auto asr_started = std::chrono::steady_clock::now();
    const auto generation = engine_.generate(audio, base_prompt_ + inference_prefix, max_tokens);
    const auto asr_finished = std::chrono::steady_clock::now();

    raw_decoded_ = normalize_generation(inference_prefix + generation.text);
    auto [language, text] = parse_output(raw_decoded_);
    if (!language.empty()) {
        detected_language_ = language;
    }
    if (settings_.language == "Chinese" || detected_language_ == "Chinese") {
        raw_decoded_ = normalize_chinese_spacing(raw_decoded_);
        text = normalize_chinese_spacing(text);
    }

    auto fixed = final ? trim(text) : fixed_prefix(raw_decoded_);
    if (automatic_language && raw_decoded_.find(asr_tag) == std::string::npos) {
        fixed.clear();
    }
    const auto bar = fixed.find('|');
    if (bar != std::string::npos) {
        fixed.resize(bar);
    }
    const auto prior = trim(committed);
    fixed = trim(fixed);
    std::string delta;
    if (starts_with(fixed, prior)) {
        delta = fixed.substr(prior.size());
    }

    if (!final) {
        const auto steady_tokens = std::max(
            1.0,
            static_cast<double>(settings_.chunk_ms * static_cast<std::uint32_t>(sample_rate) / 1000) / 1280.0);
        if (!delta.empty()) {
            max_tokens_ = steady_tokens;
            last_emitted_token_chinese_ = contains_chinese(delta);
        } else if (last_emitted_token_chinese_) {
            max_tokens_ = steady_tokens;
        } else {
            max_tokens_ += 0.5;
        }
        if (last_emitted_token_chinese_) max_tokens_ *= 2.0;
        max_tokens_ = std::min(max_tokens_, static_cast<double>(max_tokens_ceiling_));
    }

    slices_.push_back({new_audio.size(), delta});
    const auto total_finished = std::chrono::steady_clock::now();
    return {
        std::move(delta),
        final,
        std::chrono::duration<double, std::milli>(asr_finished - asr_started).count(),
        std::chrono::duration<double, std::milli>(total_finished - total_started).count(),
    };
}

void R2T2Session::append_audio(std::span<const float> audio) {
    audio_window_.insert(audio_window_.end(), audio.begin(), audio.end());
}

void R2T2Session::trim_window() {
    const auto max_samples = static_cast<std::size_t>(settings_.max_window_ms) * sample_rate / 1000;
    if (audio_window_.size() - audio_begin_ <= max_samples) {
        return;
    }
    const auto discard_target = static_cast<std::size_t>(settings_.discard_window_ms) * sample_rate / 1000;
    std::size_t discarded = 0;
    while (!slices_.empty() && discarded < discard_target) {
        discarded += slices_.front().samples;
        slices_.pop_front();
    }
    if (discarded == 0 || discarded > audio_window_.size() - audio_begin_) {
        throw std::runtime_error("rolling audio and text slices lost synchronization");
    }
    audio_begin_ += discarded;
    audio_window_.erase(audio_window_.begin(), audio_window_.begin() + static_cast<std::ptrdiff_t>(audio_begin_));
    audio_begin_ = 0;
}

std::string R2T2Session::window_prefix() const {
    std::size_t size = 0;
    for (const auto & slice : slices_) {
        size += slice.fixed_delta.size();
    }
    std::string prefix;
    prefix.reserve(size);
    for (const auto & slice : slices_) {
        prefix += slice.fixed_delta;
    }
    return prefix;
}

std::string R2T2Session::build_base_prompt() const {
    std::string context;
    if (settings_.smooth) {
        context = "Smooth the text";
    }
    if (!settings_.context.empty()) {
        if (!context.empty()) context += '\n';
        context += settings_.context;
    }
    std::string prompt = "<|im_start|>system\n" + context +
        "<|im_end|>\n<|im_start|>user\n"
        "<|audio_start|><|audio_pad|><|audio_end|>"
        "<|im_end|>\n<|im_start|>assistant\n";
    if (settings_.language != "Auto" && settings_.language != "None") {
        prompt += "language " + settings_.language + std::string(asr_tag);
    }
    return prompt;
}

std::string R2T2Session::normalize_generation(std::string value) const {
    std::size_t position = 0;
    while ((position = value.find(replacement, position)) != std::string::npos) {
        value.erase(position, replacement.size());
    }
    value = normalize_punctuation(value);
    const auto bar = value.find('|');
    if (bar != std::string::npos) {
        value.resize(bar);
    }
    return value;
}

std::string R2T2Session::fixed_prefix(std::string_view raw) const {
    auto tokens = engine_.tokenize(raw);
    std::size_t rollback = settings_.unfixed_tokens;
    if (settings_.mode == RecognitionMode::fast && ends_with_commit_punctuation(raw)) {
        rollback = 0;
    }
    if (raw.find(asr_tag) != std::string_view::npos &&
        raw.substr(raw.find(asr_tag) + asr_tag.size()).empty()) {
        rollback = 0;
    }
    while (true) {
        const auto keep = tokens.size() > rollback ? tokens.size() - rollback : 0;
        const auto decoded = engine_.detokenize(std::span<const std::int32_t>(tokens.data(), keep));
        if (decoded.find(replacement) == std::string::npos || keep == 0) {
            auto fixed = decoded;
            const auto tag = fixed.find(asr_tag);
            if (tag != std::string::npos) {
                fixed.erase(0, tag + asr_tag.size());
            }
            return fixed;
        }
        ++rollback;
    }
}

std::pair<std::string, std::string> R2T2Session::parse_output(std::string_view raw) const {
    const bool automatic_language = settings_.language == "Auto" || settings_.language == "None";
    if (!automatic_language) {
        return {settings_.language, trim(raw)};
    }
    const auto tag = raw.find(asr_tag);
    if (tag == std::string_view::npos) {
        return {{}, {}};
    }
    const auto metadata = raw.substr(0, tag);
    auto text = trim(raw.substr(tag + asr_tag.size()));
    if (metadata.find("language None") != std::string_view::npos) {
        return {{}, std::move(text)};
    }
    std::string language;
    const auto marker = metadata.find("language ");
    if (marker != std::string_view::npos) {
        const auto begin = marker + 9;
        const auto end = metadata.find_first_of("\r\n", begin);
        language = trim(metadata.substr(begin, end == std::string_view::npos ? end : end - begin));
    }
    return {std::move(language), std::move(text)};
}

} // namespace r2t2
