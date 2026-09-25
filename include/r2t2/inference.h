#pragma once

#include "r2t2/config.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace r2t2 {

struct Generation {
    std::string text;
    std::vector<std::int32_t> token_ids;
    bool stopped = false;
};

struct RuntimeInfo {
    std::string backend;
    std::vector<std::string> devices;
};

class InferenceEngine {
public:
    virtual ~InferenceEngine() = default;

    virtual Generation generate(
        std::span<const float> audio,
        std::string_view prompt,
        std::uint32_t max_tokens) = 0;
    virtual std::vector<std::int32_t> tokenize(std::string_view text) const = 0;
    virtual std::string detokenize(std::span<const std::int32_t> tokens) const = 0;
};

class BackendRuntime final {
public:
    explicit BackendRuntime(bool verbose = false);
    ~BackendRuntime();
    BackendRuntime(const BackendRuntime &) = delete;
    BackendRuntime & operator=(const BackendRuntime &) = delete;

private:
    bool verbose_ = false;
};

std::unique_ptr<InferenceEngine> load_inference_engine(
    const ModelPaths & paths,
    const InferenceOptions & options);
RuntimeInfo probe_runtime();

} // namespace r2t2
