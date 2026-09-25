# Architecture

## Process boundary

Electron starts one `r2t2-ws` process and reads one readiness JSON document from stdout. Audio and recognition data then use WebSocket messages. The process keeps the model and the multimodal projector loaded until shutdown.

The implementation has three layers:

1. `WebSocketServer` validates the connection and translates WebSocket messages.
2. `R2T2Session` owns per-connection audio, stable text, lookahead, token rollback, and EOS state.
3. `InferenceEngine` owns llama.cpp, mtmd, the model, the context, and the projector.

The transport depends on the `InferenceEngine` interface, not llama.cpp types. A future llama.cpp upgrade stays inside `src/inference.cpp` unless the inference contract changes.

## Native inference path

`LlamaMtmdEngine::generate` follows the official `r2t2_llama/native_ext.cpp` path:

1. Clear the llama context memory for the next bounded decode.
2. Replace the Qwen audio placeholder with the mtmd media marker.
3. Convert float32 16 kHz audio into an mtmd audio bitmap.
4. Tokenize the text and audio input with mtmd.
5. Run the audio encoder and multimodal projector.
6. Decode the projected audio embeddings and the text prompt into llama.cpp.
7. Generate with greedy sampling.

No Python binding remains. RAII objects release the sampler, mtmd chunks, bitmap, mtmd context, llama context, and model in dependency order.

## Streaming state

The official `streaming_transcribe_no_reset` implementation does not keep an ever-growing llama KV cache. Each iteration re-feeds a bounded audio window and a stable text prefix. This project preserves that behavior.

The first decode consumes `chunk_ms + lookahead_ms`. Later decodes consume `chunk_ms`. With the defaults, the schedule is one 320 ms decode followed by 160 ms decodes.

In `slow` mode, each non-final decode rolls back `unfixed_token_num` tokenizer tokens. In `fast` mode, text that ends with punctuation skips that rollback. Only the remaining fixed prefix can become a client delta. If the model tries to revise committed text, the session emits an empty delta for that iteration. It never sends a replacement for text already emitted.

The token budget follows the official WebSocket policy. It starts from the first chunk duration, resets after new text, grows after stalled English output, doubles after Chinese output, and remains within the upstream ceiling.

## Long sessions

The session keeps at most about 16 seconds of decoded audio. When a new chunk exceeds that window, the session discards about 8 seconds from the front. It removes the stable text slices that correspond to the same audio samples.

This paired audio and text roll preserves prompt alignment. Raw audio and temporary mtmd objects do not grow with meeting length. Transport output also remains delta-only, so client traffic does not grow quadratically.

## Concurrency and ownership

IXWebSocket gives each connection a worker thread. The server admits one active ASR connection because one reusable llama context is not safe for concurrent decoding. A move-safe connection lease owns the busy flag and releases it on EOS, close, transport error, or exception.

The model is not duplicated per connection. A new connection creates only `R2T2Session` state and reuses `InferenceEngine`.

## Shutdown

Signal handlers only set a `sig_atomic_t` flag. The main thread stops the listener outside the signal handler. Destruction then releases connection state, the WebSocket server, the inference engine, and the llama backend in that order.
