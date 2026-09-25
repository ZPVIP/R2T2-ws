# WebSocket protocol reference

## Endpoint

- Scheme: `ws`
- Default host: `127.0.0.1`
- Default port: `8272`
- Native path: `/asr_stream_api_v1`
- RStream-compatible path: `/asr`
- TLS: not supported
- Active ASR sessions: one

IXWebSocket handles RFC 6455 handshake, masking, fragmentation, control frames, and message assembly.

The path selects the wire format. Both paths use the same inference engine and audio format.

## Optional native header

The official client sends a JSON text message before audio. This server accepts that header and also accepts binary audio as the first message.

```json
{
  "requestId": "550e8400-e29b-41d4-a716-446655440000",
  "channels": 1,
  "sample_rate": 16000,
  "language": "Chinese",
  "use_vad": false,
  "system_prompt": "optional names and hot words",
  "mode": "slow",
  "smooth": false
}
```

Fields have these meanings:

| Field | Required | Meaning |
|---|---:|---|
| `requestId` | No | Client correlation ID. The server creates a UUID when omitted. |
| `channels` | No | Must be `1` when present. |
| `sample_rate` | No | Must be `16000` when present. |
| `language` | No | Overrides the CLI language. `zhen` maps to `Auto`. |
| `use_vad` | No | Must be `false`. Native VAD is not implemented. |
| `system_prompt` | No | Per-session context, limited to 4000 bytes. |
| `mode` | No | `slow` uses the configured rollback token count. `fast` commits all trailing tokens after punctuation. The CLI default is `slow`. |
| `smooth` | No | Adds `Smooth the text` before the recognition context. The CLI default is `false`. |

The server replies:

```json
{"status":"connected","requestId":"550e8400-e29b-41d4-a716-446655440000","msg":"","active_connections":1}
```

For binary-first clients, the server creates default session settings and sends the same connected response before recognition updates.

## RStream-compatible header

Clients that connect to `/asr` can send this initial JSON message:

```json
{
  "requestId": "550e8400-e29b-41d4-a716-446655440000",
  "lang": "cn",
  "booked_words": "Technical words: Tiginal, R2T2",
  "use_vad": false,
  "smooth": true
}
```

The server maps RStream language codes to model language names. It uses `booked_words` as the per-session recognition context. `mode` accepts `slow` or `fast`. `smooth` must be a Boolean. Both fields override their command-line defaults for the session.

## Audio messages

Send binary WebSocket messages with these properties:

- Signed 16-bit little-endian PCM
- 16,000 samples per second
- One channel

A common message contains 2560 samples and is 5120 bytes. Message size does not need to equal one inference chunk. The server buffers partial chunks and can consume multiple chunks from one message.

An odd binary byte count returns `INVALID_AUDIO` and closes the connection.

## Recognition updates

Each completed inference chunk returns one JSON text message:

```json
{
  "status": "success",
  "requestId": "550e8400-e29b-41d4-a716-446655440000",
  "msg": {
    "text": "new stable text only",
    "reset": false,
    "asr_cost_ms": 178.3,
    "total_cost_ms": 178.4
  }
}
```

`msg.text` is the new stable delta. Concatenate the field in arrival order. Empty deltas are valid.

On `/asr`, `msg.text` contains the cumulative stable transcript and the top-level `is_final` field marks the last update:

```json
{
  "status": "success",
  "requestId": "550e8400-e29b-41d4-a716-446655440000",
  "is_final": true,
  "msg": {
    "text": "the complete stable transcript",
    "asr_cost_ms": 178.3,
    "total_cost_ms": 178.4
  }
}
```

`asr_cost_ms` measures native mtmd and llama.cpp inference for that update. `total_cost_ms` measures session processing from the start of the decode through stable-prefix processing. It excludes WebSocket serialization and socket write time.

## EOS

Native clients send this text message:

```text
YOUDAO_ONETIME_ASR_STREAM_EOS
```

RStream clients send this text message:

```text
YOUDAO_ASR_EOS
```

The server accepts either EOS command on either path.

If an incomplete audio chunk remains, the server decodes it without padding. The native response uses `reset: true`. The RStream response uses `is_final: true`. The server then closes with WebSocket code `1000`.

If EOS arrives on an exact inference boundary, the final text delta is empty. This matches the upstream `finish_streaming_transcribe_no_reset` behavior.

EOS before audio returns `EOS_BEFORE_AUDIO`.

## Authentication

Authentication is off by default for official-client compatibility.

`--auth auto` creates a 256-bit random hexadecimal token and adds it to the readiness object:

```json
{
  "host": "127.0.0.1",
  "port": 41837,
  "endpoint": "/asr_stream_api_v1",
  "url": "ws://127.0.0.1:41837/asr_stream_api_v1?token=...",
  "rstreamEndpoint": "/asr",
  "rstreamUrl": "ws://127.0.0.1:41837/asr?t=...",
  "authToken": "...",
  "event": "ready"
}
```

The server also prints a human-readable message to stderr:

```text
Native WebSocket URL:
ws://127.0.0.1:41837/asr_stream_api_v1?token=...

RStream-compatible WebSocket URL:
ws://127.0.0.1:41837/asr?t=...

Server ready
```

Parent processes must collect and parse the complete JSON document from stdout. Human-readable messages and diagnostics use stderr.

Native clients can connect with the `token` query parameter:

```text
ws://127.0.0.1:41837/asr_stream_api_v1?token=<authToken>
```

RStream clients can connect with the `t` query parameter:

```text
ws://127.0.0.1:41837/asr?t=<authToken>
```

The server accepts both query parameter names on both paths.

`--auth <token>` uses a fixed token. `--auth off` disables validation.

## Errors

Errors use this shape:

```json
{
  "status": "error",
  "requestId": "550e8400-e29b-41d4-a716-446655440000",
  "error": {
    "code": "INVALID_AUDIO",
    "message": "PCM binary message must contain an even number of bytes"
  }
}
```

The server can return these codes:

| Code | Condition |
|---|---|
| `INVALID_PATH` | The request URI has the wrong path. |
| `UNAUTHORIZED` | The local token is missing or invalid. |
| `BUSY` | Another ASR session is active. |
| `INVALID_HEADER` | The optional first JSON message is malformed or unsupported. |
| `INVALID_AUDIO` | A binary message has an odd byte count. |
| `UNEXPECTED_COMMAND` | A non-EOS text message arrives after streaming begins. |
| `EOS_BEFORE_AUDIO` | EOS arrives before any samples. |
| `INFERENCE_FAILED` | mtmd, llama.cpp, or stable-state processing failed. |
