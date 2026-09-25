# Known limitations

- Only macOS Apple Silicon with Metal has a completed hardware validation.
- The server accepts one active ASR session. It shares one loaded model and one llama context.
- Native VAD is not implemented. The client must send EOS.
- Input must already be 16 kHz, mono, signed 16-bit little-endian PCM.
- The server does not decode WAVE, MP3, AAC, or other containers. The example client extracts PCM from WAVE before sending it.
- `--port 0` uses IXWebSocket's free-port helper before binding. Another process can claim the selected port during that short interval.
- The pinned mtmd audio encoder is marked experimental by llama.cpp. The official project recommends its hybrid PyTorch encoder for the best quiet-onset accuracy, but that route is outside the Python-free runtime requirement.
- The implementation follows upstream `streaming_transcribe_no_reset`: it clears llama memory for each decode and re-feeds a bounded 16-second audio window with stable text state. It does not keep a persistent cross-chunk llama KV cache.
- The official Python parity run needs a separate development environment. It is not part of the native runtime.
- `total_cost_ms` excludes WebSocket JSON serialization and socket transmission.
- Local token authentication protects Electron IPC from unrelated localhost clients. It does not provide TLS or remote-network security.
