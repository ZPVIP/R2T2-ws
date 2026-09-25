class R2t2Ws < Formula
  desc "Native Confucius4-R2T2 ASR server with two WebSocket interfaces"
  homepage "https://github.com/ZPVIP/R2T2-ws"
  license "GPL-3.0-only"
  head "https://github.com/ZPVIP/R2T2-ws.git", branch: "main"

  depends_on "cmake" => :build

  resource "llama.cpp" do
    url "https://github.com/ggml-org/llama.cpp/archive/ad6c66839af3c5646fba8c6c2e2087a1e4e38948.tar.gz"
    sha256 "fb02c93eef3b4b13e8bfd1a241f3f18a91ad926ec51cc32fb4c9a2bfe6f37208"
  end

  resource "ixwebsocket" do
    url "https://github.com/machinezone/IXWebSocket/archive/64fae7676bd8fe31f7cb4bcde7a6841892dad65e.tar.gz"
    sha256 "af641277dd79c8f0970a31b84beec223d8c683eb8a671b0cce20bd8a8f750878"
  end

  resource "nlohmann-json" do
    url "https://github.com/nlohmann/json/archive/55f93686c01528224f448c19128836e7df245f72.tar.gz"
    sha256 "67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef"
  end

  def install
    vendor = buildpath/"vendor"
    vendor.mkpath
    resource("llama.cpp").stage vendor/"llama.cpp"
    resource("ixwebsocket").stage vendor/"ixwebsocket"
    resource("nlohmann-json").stage vendor/"nlohmann-json"

    args = std_cmake_args + %W[
      -DR2T2_BUILD_TESTS=OFF
      -DR2T2_ENABLE_METAL=#{OS.mac? ? "ON" : "OFF"}
      -DR2T2_LLAMA_CPP_SOURCE_DIR=#{vendor}/llama.cpp
      -DR2T2_IXWEBSOCKET_SOURCE_DIR=#{vendor}/ixwebsocket
      -DR2T2_NLOHMANN_JSON_SOURCE_DIR=#{vendor}/nlohmann-json
    ]

    system "cmake", "-S", ".", "-B", "build", *args
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"
  end

  def caveats
    <<~EOS
      This Formula installs one executable named r2t2-ws.

      The server exposes two WebSocket interfaces:
        ws://127.0.0.1:8272/asr_stream_api_v1  Confucius4-R2T2 native
        ws://127.0.0.1:8272/asr                RStream-compatible, used by the web demo and Tiginal

      Run r2t2-ws with --auth auto to print authenticated URLs for both interfaces.
      Model weights are not included.
    EOS
  end

  test do
    assert_match '"name":"r2t2-ws"', shell_output("#{bin}/r2t2-ws --probe")
    assert_match "r2t2-ws 0.1.0", shell_output("#{bin}/r2t2-ws --version")
  end
end
