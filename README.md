# R2T2-ws

`R2T2-ws` is the repository and project name. The project installs one user-facing executable named `r2t2-ws`. Test builds also create internal test runners, which are not installed.

`r2t2-ws` is a local C++ streaming ASR helper for Electron applications. It loads Confucius4-R2T2 GGUF weights once and runs the official `stream_llama` path through llama.cpp and mtmd.

The executable supports both the official Confucius4-R2T2 server protocol and the WebSocket interface used by the public R2T2 web demo. Both interfaces run inside the same `r2t2-ws` process.

The executable does not use Python, PyTorch, vLLM, transformers, pybind11, `llama-cli`, or `llama-server` at runtime.

## Status

The implementation is validated on macOS Apple Silicon with Metal. The source and CMake options also support macOS Intel, Linux x64, and Windows x64. Those three targets still need CI or hardware validation.

The server supports one active ASR session. A second connection receives a structured `BUSY` error. The loaded model remains available after a client disconnects.

## Two compatible WebSocket interfaces

The single `r2t2-ws` process exposes two WebSocket interfaces:

| Interface | Path | Upstream reference |
|---|---|---|
| Confucius4-R2T2 native | `/asr_stream_api_v1` | The official [`run_start_server.sh`](https://github.com/netease-youdao/Confucius4-R2T2/blob/master/run_start_server.sh) script starts [`ws_server.py`](https://github.com/netease-youdao/Confucius4-R2T2/blob/master/ws_server.py), which defines this WebSocket endpoint. |
| RStream-compatible | `/asr` | The public [R2T2 web demo](https://r2t2.youdao.com/demo) connects to this interface. |

The native interface uses fields such as `language` and `system_prompt`, returns stable text deltas, and ends with `YOUDAO_ONETIME_ASR_STREAM_EOS`. The RStream-compatible interface uses `lang` and `booked_words`, returns the cumulative stable transcript with `is_final`, and ends with `YOUDAO_ASR_EOS`.

Both interfaces use the same loaded model and the same `r2t2-ws` process. Tiginal uses the RStream-compatible `/asr` interface, so its provider list only needs one `R2T2` choice.

### RStream language codes

The `/asr` interface accepts the language codes used by the public demo:

| Language | Code | Accepted aliases | Language | Code | Accepted aliases |
|---|---|---|---|---|---|
| Chinese | `cn` | `chinese` | Cantonese | `yue` | `cantonese` |
| English | `en` | `english`, `enzh`, `eng`, `enus` | Japanese | `jp` | `japanese` |
| Korean | `ko` | `korean` | French | `fr` | `french` |
| German | `de` | `german` | Spanish | `es` | `spanish`, legacy `sp` |
| Italian | `it` | `italian` | Russian | `ru` | `russian` |
| Portuguese | `pt` | `portuguese` | Thai | `th` | `thai` |
| Vietnamese | `vi` | `vietnamese` | Indonesian | `id` | `indonesian` |
| Turkish | `tr` | `turkish` | Hindi | `hi` | `hindi` |
| Malay | `ms` | `malay` | Dutch | `nl` | `dutch` |
| Swedish | `sv` | `swedish` | Danish | `da` | `danish` |
| Finnish | `fi` | `finnish` | Polish | `pl` | `polish` |
| Czech | `cs` | `czech` | Filipino | `fil` | `filipino` |
| Persian | `fa` | `persian` | Greek | `el` | `greek` |
| Romanian | `ro` | `romanian` | Hungarian | `hu` | `hungarian` |
| Macedonian | `mk` | `macedonian` | Arabic | `ar` | `arabic` |

Tiginal normalizes these names and aliases before opening a session. New clients should send `es` for Spanish. The server still accepts `sp` for older clients.

### Public demo compatibility notes

The public demo sends mono 16 kHz PCM in 160 ms WebSocket messages and ends an RStream session with `YOUDAO_ASR_EOS`. Its client limits technical words to 50 characters and formats them as `Technical words: ...` in `booked_words`.

The demo also implements browser-side flow control: a conservative three-packet initial window, adaptive growth up to twelve packets, a 512 KiB socket high-water mark, ACK-stall detection, and bounded reconnect attempts. These values describe the demo client, not additional requirements imposed by `r2t2-ws`. The demo deployment currently advertises a 30-second session limit; a local `r2t2-ws` process does not add that limit.

## Install with Homebrew

The repository includes a HEAD-only Formula. Install it from the custom tap:

```bash
brew tap ZPVIP/r2t2 https://github.com/ZPVIP/R2T2-ws.git
brew install --HEAD ZPVIP/r2t2/r2t2-ws
```

Homebrew builds the native executable with Metal and installs it in the Homebrew `bin` directory. It does not install model weights. See [Install with Homebrew](docs/homebrew.md) for local Formula testing and stable release steps.

The Formula downloads pinned llama.cpp, IXWebSocket, and nlohmann/json source archives as build resources. They are compilation dependencies, not separate commands that users must install. The installed `r2t2-ws` executable contains the required native code, but the Confucius4-R2T2 model and `mmproj` files must still be downloaded separately.

## Get the model

Download one language model and one compatible `mmproj` file from the official [Confucius4-R2T2-GGUF repository](https://huggingface.co/netease-youdao/Confucius4-R2T2-GGUF). The tested desktop combination is:

- `Confucius4-R2T2-Q4_K_M.gguf`
- `mmproj-Confucius4-R2T2-Q8_0.gguf`

Keep model weights outside this repository. The weights use the NetEase model license, not this project's GPL license.

<details>
<summary>Build on macOS Apple Silicon</summary>

Install CMake 3.24 or newer and an Xcode command-line toolchain. Metal is enabled by default on macOS.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
ctest --test-dir build --output-on-failure
```

The build fetches exact dependency revisions. To build without network access, pass existing source trees:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DR2T2_LLAMA_CPP_SOURCE_DIR=/path/to/llama.cpp \
  -DR2T2_IXWEBSOCKET_SOURCE_DIR=/path/to/IXWebSocket \
  -DR2T2_NLOHMANN_JSON_SOURCE_DIR=/path/to/json
cmake --build build -j
```

Each source tree must match the revision in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

</details>

<details>
<summary>Build on macOS Intel</summary>

Use the Apple Silicon commands on an Intel Mac. Metal remains enabled by default. To build a CPU-only binary, disable Metal:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DR2T2_ENABLE_METAL=OFF
cmake --build build -j
```

Do not use one build directory for both `arm64` and `x86_64`. To create separate binaries from one host toolchain, set `CMAKE_OSX_ARCHITECTURES` in separate build directories.

</details>

<details>
<summary>Build on Linux x64</summary>

The default Linux build uses the CPU backend:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

To enable CUDA, install a compatible CUDA toolkit and configure:

```bash
cmake -S . -B build-cuda \
  -DCMAKE_BUILD_TYPE=Release \
  -DR2T2_ENABLE_CUDA=ON
cmake --build build-cuda -j
```

To enable Vulkan, install a Vulkan SDK and set `-DR2T2_ENABLE_VULKAN=ON` instead.

</details>

<details>
<summary>Build on Windows x64</summary>

Open an x64 Native Tools Command Prompt for Visual Studio 2022. The default build uses the CPU backend:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

For an NVIDIA build, add `-DR2T2_ENABLE_CUDA=ON` during configuration. For Vulkan, add `-DR2T2_ENABLE_VULKAN=ON`. Do not enable Metal on Windows.

</details>

## Publish the Homebrew Formula

The custom tap maps `ZPVIP/r2t2` to this repository. Homebrew reads `Formula/r2t2-ws.rb` from the remote default branch, and the Formula builds the source from `main`.

To publish the current HEAD-only Formula:

1. Commit the source changes, `Formula/r2t2-ws.rb`, and the Homebrew documentation.
2. Push the branch and merge it into `main`.
3. Confirm that [Formula/r2t2-ws.rb](Formula/r2t2-ws.rb) is available on the GitHub `main` branch.
4. Install the Formula on a target Mac with the commands in [Install with Homebrew](#install-with-homebrew).

`git push` does not upload uncommitted working-tree changes. Pushing only a feature branch also does not publish this tap because Homebrew reads the remote default branch.

After a new commit reaches `main`, update an existing tap and rebuild the executable:

```bash
brew update
brew reinstall --HEAD ZPVIP/r2t2/r2t2-ws
```

## Start the server

Pass explicit paths:

```bash
./build/r2t2-ws \
  --model /path/to/Confucius4-R2T2-Q4_K_M.gguf \
  --mmproj /path/to/mmproj-Confucius4-R2T2-Q8_0.gguf \
  --host 127.0.0.1 \
  --port 8272 \
  --language Chinese \
  --chunk-ms 160 \
  --mode slow \
  --auth auto
```

Alternatively, place exactly one main GGUF and one `mmproj*.gguf` in a directory:

```bash
./build/r2t2-ws --gguf-dir /path/to/models --port 8272
```

An interactive terminal shows the loading status, the readiness document, and both connection URLs with blank lines between sections:

```text
Loading language model   [##############################] 100%
Loading audio projector  [##############################] 100%
Model loaded in 2059.14 ms

{
  "host": "127.0.0.1",
  "port": 8272,
  "endpoint": "/asr_stream_api_v1",
  "url": "ws://127.0.0.1:8272/asr_stream_api_v1",
  "rstreamEndpoint": "/asr",
  "rstreamUrl": "ws://127.0.0.1:8272/asr",
  "event": "ready"
}

Native WebSocket URL:
ws://127.0.0.1:8272/asr_stream_api_v1

RStream-compatible WebSocket URL:
ws://127.0.0.1:8272/asr

Server ready
```

The process prints the two-space-indented readiness JSON document to stdout for Electron. Loading progress, connection URLs, and diagnostics use stderr. Electron must collect and parse the complete JSON document from stdout.

With `--auth auto`, the readiness document includes the generated token:

```json
{
  "host": "127.0.0.1",
  "port": 8272,
  "endpoint": "/asr_stream_api_v1",
  "url": "ws://127.0.0.1:8272/asr_stream_api_v1?token=generated-token",
  "rstreamEndpoint": "/asr",
  "rstreamUrl": "ws://127.0.0.1:8272/asr?t=generated-token",
  "authToken": "generated-token",
  "event": "ready"
}
```

The default log level hides llama.cpp and mtmd warnings. Use `--verbose` or `--debug` to show upstream diagnostic messages. Loading errors always remain visible. Progress bars do not appear when another process redirects stderr.

Use `--debug` to trace WebSocket input and output on stderr. Text messages and JSON responses are shown in full. Binary audio logs include the byte count, sample count, and a 16-byte hexadecimal preview. The debug trace does not repeat the authentication token. The ready URL prints it once so that you can copy the connection address.

Use `--port 0` to select an available port. Use `--auth auto` for Electron IPC. The readiness object includes `authToken`, `url`, and `rstreamUrl`. Tiginal connects with `rstreamUrl`.

To use a fixed token, pass it after `--auth`:

```bash
./build/r2t2-ws \
  --gguf-dir /path/to/models \
  --auth your-secret-token
```

Use `--auth off` only when another trusted local process controls access to the server. Authentication is off by default for compatibility with the official client.

### Recognition controls

Recognition controls apply to one WebSocket session. The opening JSON header overrides the command-line default for that connection.

| Control | C++ behavior | Command-line default |
|---|---|---|
| `mode` | `slow` keeps the configured trailing rollback token count. `fast` commits all trailing tokens when the current text ends with punctuation. | `--mode slow` or `--mode fast` |
| `smooth` | `true` adds `Smooth the text` before the recognition context, matching the official Python server. It is a model prompt, not deterministic text post-processing. | `--smooth` enables it. The default is off. |
| `use_vad` | Only `false` is supported. Clients must send an EOS message to finish a session. | No option is provided because native VAD is not implemented. |

The official Python server loads FireRedVAD from a separate model path. The C++ server currently loads only the R2T2 language model and its audio projector. Adding VAD requires another model, inference runtime, streaming state, and speech-end reset behavior. The R2T2 GGUF and `mmproj` files do not contain FireRedVAD.

## Test with the Node client

Node.js 22 or newer provides the WebSocket API used by the example. Electron renderer processes can use the same message flow.

```bash
node examples/node-electron-client.mjs \
  ws://127.0.0.1:8272/asr_stream_api_v1 \
  /path/to/16k-mono-pcm16.wav
```

Add `--realtime` to wait 160 ms between frames. Without the flag, the client sends the file as fast as the server accepts it.

## Probe the runtime

`--probe` does not need model files or Python:

```bash
./build/r2t2-ws --probe
```

Example output:

```json
{"backend":"metal","channels":1,"devices":["Metal GPU","BLAS","CPU"],"llama_cpp_commit":"ad6c66839af3c5646fba8c6c2e2087a1e4e38948","name":"r2t2-ws","r2t2":true,"sample_format":"s16le","sample_rate":16000,"version":"0.1.0"}
```

## CLI options

Run `r2t2-ws --help` for the complete option list. The main streaming controls are:

| Option | Meaning |
|---|---|
| `--language Chinese` or `--language Auto` | Sets the default recognition language. A WebSocket header can override it. |
| `--chunk-ms 160` | Decodes each steady-state 160 ms audio chunk. Valid values range from 80 through 2000 ms. Smaller chunks reduce update latency and run inference more often. |
| `--lookahead-ms 160` | Adds 160 ms only to the first decode. With the default chunk size, the first decode receives 320 ms. More lookahead gives the model more starting context but delays the first result. |
| `--unfixed-token-num 1` | Keeps the last token provisional instead of emitting it as stable text. A larger value is more conservative and increases text latency. `fast` mode commits the trailing tokens when the text ends with punctuation. |
| `--context "optional names and hot words"` | Sets the default model context for names, terminology, and hot words. Native `system_prompt` or RStream `booked_words` replaces it for one session. Context biases recognition but does not guarantee a spelling. |
| `--mode slow` or `--mode fast` | Selects the stable-text commit rule described in [Recognition controls](#recognition-controls). |
| `--smooth` | Adds the upstream smooth-text prompt to sessions that do not override `smooth`. |
| `--threads <count>` | Sets the CPU worker count for llama.cpp and the audio projector. The default is the number of hardware threads. More threads do not always improve Metal or CUDA performance. |
| `--gpu-layers -1` | Offloads all supported language-model layers to the GPU. Use `0` to keep those layers on the CPU. The default is `-1`. |
| `--auth off`, `--auth auto`, or `--auth <token>` | Controls local WebSocket authentication. |
| `--debug` | Enables verbose diagnostics and traces WebSocket input and output on stderr. Text and JSON appear in full. Binary messages show their byte count, sample count, and first 16 bytes. |

Audio must be mono, 16 kHz, signed 16-bit little-endian PCM.

## Documentation

- [Architecture](docs/architecture.md) explains native inference, stable text, bounded state, and ownership.
- [Protocol reference](docs/protocol.md) defines connection, audio, output, errors, EOS, and authentication.
- [Testing](docs/testing.md) describes unit, integration, and official Python parity tests.
- [Homebrew installation](docs/homebrew.md) describes the Formula and release workflow.
- [Third-party notices](THIRD_PARTY_NOTICES.md) lists pinned dependencies and licenses.
- [Known limitations](docs/known-limitations.md) records current platform and behavior limits.
