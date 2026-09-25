# Test the native helper

## Run deterministic tests

Build with `R2T2_BUILD_TESTS=ON`, then run CTest:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DR2T2_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`tests/unit_tests.cpp` checks PCM parsing, header validation, recognition mode, the smooth prompt, first-chunk lookahead, variable frame buffering, token rollback, delta-only output, model prefix revision handling, EOS tail decoding, exact-boundary EOS, automatic language gating, and the 16-second audio window with an 8-second discard.

CTest also runs `tests/websocket_tests.cpp`. It starts an in-process server, verifies protocol errors and EOS behavior, and opens two sequential sessions to confirm that the single-session lease is released correctly.

## Run a manual WebSocket integration test

Start `r2t2-ws`, then run:

```bash
node examples/node-electron-client.mjs \
  ws://127.0.0.1:8272/asr_stream_api_v1 \
  /path/to/Confucius4-R2T2/resources/test.wav
```

The client validates the WAVE format, sends 160 ms binary frames, prints every response, sends EOS, concatenates deltas, and prints the final transcript.

## Compare with the official Python implementation

The parity test is a development tool. The native executable does not use Python.

Create a Python environment that can run the official `stream_llama` example. The official macOS repository does not provide a prebuilt native extension, so macOS parity requires building that extension and installing its Python dependencies.

Capture the official output:

```bash
cd /path/to/Confucius4-R2T2
GGUF_DIR=/path/to/models python -m r2t2_llama.example_llama \
  --audio resources/test.wav \
  --model_path /path/to/Confucius4-R2T2-HF \
  --infer_mode stream_llama \
  --language Chinese \
  --chunk_size_ms 160 \
  --lookahead_ms 160 \
  --unfixed_token_num 1 | tee /tmp/official-r2t2.txt
```

Capture the native client output from a running server:

```bash
node examples/node-electron-client.mjs \
  ws://127.0.0.1:8272/asr_stream_api_v1 \
  /path/to/Confucius4-R2T2/resources/test.wav | tee /tmp/native-r2t2.txt
```

Compare both streams:

```bash
python3 tests/parity_test.py \
  --official-output /tmp/official-r2t2.txt \
  --native-output /tmp/native-r2t2.txt
```

The comparator derives append-only official deltas from `text=` lines, extracts native `msg.text` values, checks the final transcript, and reports emission counts. A mismatch exits with a nonzero status.

## Validated Apple Silicon result

The validated machine used an Apple M5 GPU, Q4_K_M language weights, a Q8_0 projector, and the official 6.74-second `resources/test.wav` file.

- Native transcript: a complete Chinese sentence with append-only deltas and final punctuation
- Model load after Metal initialization: 3.60 seconds in the final measured run
- First update with 320 ms lookahead: 626 ms
- Steady per-chunk inference: about 160 through 235 ms, with a 188 ms mean
- Inference realtime factor: about 1.24 for the 6.74-second sample
- Peak resident set size: 5,283,741,696 bytes
- Reported backend: `metal`

The official Python parity command could not run in the validation environment because its PyTorch, transformers, audio, and native-extension dependencies were absent. The checked-in comparator records the remaining parity step instead of claiming an unmeasured match.
