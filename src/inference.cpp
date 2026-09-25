// Adapted from Confucius4-R2T2 r2t2_llama/native_ext.cpp.
// Copyright 2026 The NetEase Youdao team.
// SPDX-License-Identifier: Apache-2.0

#include "r2t2/inference.h"

#include "ggml-backend.h"
#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace r2t2 {
namespace {

template <typename T, void (*Free)(T *)>
struct NativeDeleter {
    void operator()(T * value) const noexcept {
        if (value != nullptr) {
            Free(value);
        }
    }
};

using ModelPtr = std::unique_ptr<llama_model, NativeDeleter<llama_model, llama_model_free>>;
using ContextPtr = std::unique_ptr<llama_context, NativeDeleter<llama_context, llama_free>>;
using MtmdPtr = std::unique_ptr<mtmd_context, NativeDeleter<mtmd_context, mtmd_free>>;
using BitmapPtr = std::unique_ptr<mtmd_bitmap, NativeDeleter<mtmd_bitmap, mtmd_bitmap_free>>;
using ChunksPtr = std::unique_ptr<mtmd_input_chunks, NativeDeleter<mtmd_input_chunks, mtmd_input_chunks_free>>;
using SamplerPtr = std::unique_ptr<llama_sampler, NativeDeleter<llama_sampler, llama_sampler_free>>;

void stderr_log(enum ggml_log_level level, const char * text, void * user_data) {
    const auto verbose = *static_cast<const bool *>(user_data);
    if (text != nullptr &&
        (verbose || level == GGML_LOG_LEVEL_ERROR)) {
        std::fputs(text, stderr);
    }
}

bool stderr_is_terminal() noexcept {
#if defined(_WIN32)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

class LoadProgress final {
public:
    LoadProgress(std::string label, bool enabled)
        : label_(std::move(label)), enabled_(enabled) {
        update(0.0F);
    }

    ~LoadProgress() {
        if (enabled_ && last_percent_ < 100) {
            std::fputc('\n', stderr);
        }
    }

    LoadProgress(const LoadProgress &) = delete;
    LoadProgress & operator=(const LoadProgress &) = delete;

    static bool callback(float progress, void * user_data) {
        static_cast<LoadProgress *>(user_data)->update(progress);
        return true;
    }

    void finish() {
        update(1.0F);
    }

private:
    void update(float progress) {
        if (!enabled_) return;
        constexpr int width = 30;
        const auto bounded = std::clamp(progress, 0.0F, 1.0F);
        const auto percent = static_cast<int>(bounded * 100.0F + 0.5F);
        if (percent == last_percent_) return;
        last_percent_ = percent;
        const auto filled = percent * width / 100;
        std::fprintf(stderr, "\r%-24s [", label_.c_str());
        for (int index = 0; index < width; ++index) {
            std::fputc(index < filled ? '#' : '-', stderr);
        }
        std::fprintf(stderr, "] %3d%%", percent);
        if (percent == 100) std::fputc('\n', stderr);
        std::fflush(stderr);
    }

    std::string label_;
    bool enabled_ = false;
    int last_percent_ = -1;
};

std::string replace_audio_marker(std::string prompt) {
    constexpr std::string_view marker = "<|audio_start|><|audio_pad|><|audio_end|>";
    const auto position = prompt.find(marker);
    if (position == std::string::npos || prompt.find(marker, position + marker.size()) != std::string::npos) {
        throw std::invalid_argument("prompt must contain exactly one Qwen3-ASR audio marker");
    }
    prompt.replace(position, marker.size(), mtmd_default_marker());
    return prompt;
}

std::string token_piece(const llama_vocab * vocab, llama_token token) {
    std::string buffer(64, '\0');
    auto count = llama_token_to_piece(vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, false);
    if (count < 0) {
        buffer.resize(static_cast<std::size_t>(-count));
        count = llama_token_to_piece(vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, false);
    }
    if (count < 0) {
        throw std::runtime_error("llama_token_to_piece failed");
    }
    buffer.resize(static_cast<std::size_t>(count));
    return buffer;
}

class LlamaMtmdEngine final : public InferenceEngine {
public:
    LlamaMtmdEngine(const ModelPaths & paths, const InferenceOptions & options)
        : batch_tokens_(options.batch_tokens) {
        const auto show_progress = stderr_is_terminal();
        auto model_params = llama_model_default_params();
        model_params.n_gpu_layers = options.gpu_layers;
        LoadProgress model_progress("Loading language model", show_progress);
        if (show_progress) {
            model_params.progress_callback = LoadProgress::callback;
            model_params.progress_callback_user_data = &model_progress;
        }
        model_.reset(llama_model_load_from_file(paths.model.string().c_str(), model_params));
        if (!model_) {
            throw std::runtime_error("failed to load llama model: " + paths.model.string());
        }
        model_progress.finish();

        auto context_params = llama_context_default_params();
        context_params.n_ctx = options.context_tokens;
        context_params.n_batch = options.batch_tokens;
        context_params.n_ubatch = std::min<std::uint32_t>(options.batch_tokens, 512);
        context_params.n_seq_max = 1;
        context_params.n_threads = options.threads;
        context_params.n_threads_batch = options.threads;
        context_params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
        context_params.offload_kqv = options.use_gpu;
        context_.reset(llama_init_from_model(model_.get(), context_params));
        if (!context_) {
            throw std::runtime_error("failed to create llama context");
        }

        auto mtmd_params = mtmd_context_params_default();
        mtmd_params.use_gpu = options.use_gpu;
        mtmd_params.n_threads = options.threads;
        mtmd_params.warmup = false;
        LoadProgress projector_progress("Loading audio projector", show_progress);
        if (show_progress) {
            mtmd_params.progress_callback = LoadProgress::callback;
            mtmd_params.progress_callback_user_data = &projector_progress;
        }
        mtmd_.reset(mtmd_init_from_file(paths.mmproj.string().c_str(), model_.get(), mtmd_params));
        if (!mtmd_) {
            throw std::runtime_error("failed to load mtmd projector: " + paths.mmproj.string());
        }
        projector_progress.finish();
        if (!mtmd_support_audio(mtmd_.get())) {
            throw std::runtime_error("the mtmd projector does not support audio");
        }
    }

    Generation generate(
        std::span<const float> audio,
        std::string_view prompt,
        std::uint32_t max_tokens) override {
        std::lock_guard lock(mutex_);
        if (audio.empty()) {
            throw std::invalid_argument("audio must not be empty");
        }
        if (max_tokens == 0) {
            throw std::invalid_argument("max_tokens must be positive");
        }

        llama_memory_clear(llama_get_memory(context_.get()), true);
        auto mtmd_prompt = replace_audio_marker(std::string(prompt));
        BitmapPtr bitmap(mtmd_bitmap_init_from_audio(audio.size(), audio.data()));
        if (!bitmap) {
            throw std::runtime_error("failed to create mtmd audio bitmap");
        }

        mtmd_input_text text{mtmd_prompt.data(), mtmd_prompt.size(), true, true};
        const mtmd_bitmap * bitmaps[] = {bitmap.get()};
        ChunksPtr chunks(mtmd_input_chunks_init());
        const auto tokenize_result = mtmd_tokenize(mtmd_.get(), chunks.get(), &text, bitmaps, 1);
        if (tokenize_result != 0) {
            throw std::runtime_error("mtmd_tokenize failed with code " + std::to_string(tokenize_result));
        }

        llama_pos past = 0;
        const auto chunk_count = mtmd_input_chunks_size(chunks.get());
        for (std::size_t index = 0; index < chunk_count; ++index) {
            const auto * chunk = mtmd_input_chunks_get(chunks.get(), index);
            llama_pos new_past = past;
            int32_t result = 0;
            if (mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_TEXT) {
                result = mtmd_helper_eval_chunk_single(
                    mtmd_.get(), context_.get(), chunk, past, 0, batch_tokens_,
                    index + 1 == chunk_count, &new_past);
            } else {
                result = mtmd_encode_chunk(mtmd_.get(), chunk);
                if (result == 0) {
                    auto * embedding = mtmd_get_output_embd(mtmd_.get());
                    if (embedding == nullptr) {
                        throw std::runtime_error("mtmd returned no audio embedding");
                    }
                    result = mtmd_helper_decode_image_chunk(
                        mtmd_.get(), context_.get(), chunk, embedding,
                        past, 0, batch_tokens_, &new_past, nullptr, nullptr);
                }
            }
            if (result != 0) {
                throw std::runtime_error(
                    "failed to evaluate mtmd chunk " + std::to_string(index) +
                    ", code " + std::to_string(result));
            }
            past = new_past;
        }

        SamplerPtr sampler(llama_sampler_init_greedy());
        if (!sampler) {
            throw std::runtime_error("failed to create greedy sampler");
        }
        const auto * vocab = llama_model_get_vocab(model_.get());
        Generation generation;
        generation.token_ids.reserve(max_tokens);
        for (std::uint32_t index = 0; index < max_tokens; ++index) {
            const auto token = llama_sampler_sample(sampler.get(), context_.get(), -1);
            generation.token_ids.push_back(token);
            llama_sampler_accept(sampler.get(), token);
            if (llama_vocab_is_eog(vocab, token)) {
                generation.stopped = true;
                break;
            }
            auto next = token;
            auto batch = llama_batch_get_one(&next, 1);
            if (llama_decode(context_.get(), batch) != 0) {
                throw std::runtime_error("llama_decode failed during generation");
            }
            ++past;
        }
        for (const auto token : generation.token_ids) {
            generation.text += token_piece(vocab, token);
        }
        return generation;
    }

    std::vector<std::int32_t> tokenize(std::string_view text) const override {
        const auto * vocab = llama_model_get_vocab(model_.get());
        if (text.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
            throw std::invalid_argument("text is too large to tokenize");
        }
        std::vector<llama_token> tokens(std::max<std::size_t>(2, text.size() + 2));
        auto count = llama_tokenize(
            vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
            static_cast<int32_t>(tokens.size()), false, true);
        if (count < 0) {
            tokens.resize(static_cast<std::size_t>(-count));
            count = llama_tokenize(
                vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
                static_cast<int32_t>(tokens.size()), false, true);
        }
        if (count < 0) {
            throw std::runtime_error("llama_tokenize failed with code " + std::to_string(count));
        }
        tokens.resize(static_cast<std::size_t>(count));
        return {tokens.begin(), tokens.end()};
    }

    std::string detokenize(std::span<const std::int32_t> token_ids) const override {
        const auto * vocab = llama_model_get_vocab(model_.get());
        std::vector<llama_token> tokens(token_ids.begin(), token_ids.end());
        std::string output;
        for (const auto token : tokens) {
            output += token_piece(vocab, token);
        }
        return output;
    }

private:
    ModelPtr model_;
    ContextPtr context_;
    MtmdPtr mtmd_;
    int batch_tokens_ = 8192;
    mutable std::mutex mutex_;
};

} // namespace

BackendRuntime::BackendRuntime(bool verbose) : verbose_(verbose) {
    llama_log_set(stderr_log, &verbose_);
    mtmd_helper_log_set(stderr_log, &verbose_);
    llama_backend_init();
}

BackendRuntime::~BackendRuntime() {
    llama_backend_free();
}

std::unique_ptr<InferenceEngine> load_inference_engine(
    const ModelPaths & paths,
    const InferenceOptions & options) {
    return std::make_unique<LlamaMtmdEngine>(paths, options);
}

RuntimeInfo probe_runtime() {
    RuntimeInfo info;
    info.backend = "cpu";
    for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
        const auto device = ggml_backend_dev_get(index);
        std::string name = ggml_backend_dev_name(device);
        if (name.empty()) name = ggml_backend_dev_description(device);
        const auto type = ggml_backend_dev_type(device);
        if (name.empty() && (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU)) {
#if R2T2_METAL_ENABLED
            name = "Metal GPU";
#elif R2T2_CUDA_ENABLED
            name = "CUDA GPU";
#elif R2T2_VULKAN_ENABLED
            name = "Vulkan GPU";
#else
            name = "GPU";
#endif
        }
        info.devices.push_back(name);
        if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
            auto lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
#if R2T2_METAL_ENABLED
            info.backend = "metal";
#elif R2T2_CUDA_ENABLED
            info.backend = "cuda";
#elif R2T2_VULKAN_ENABLED
            info.backend = "vulkan";
#else
            info.backend = lower;
#endif
        }
    }
    return info;
}

} // namespace r2t2
