# Install with Homebrew

The Homebrew Formula builds `r2t2-ws` from source. It enables Metal on macOS and installs one executable named `r2t2-ws` in the Homebrew `bin` directory. It does not install model weights.

llama.cpp, IXWebSocket, and nlohmann/json are pinned build resources. Homebrew downloads their source and compiles the required code into `r2t2-ws`. Users do not need to install those libraries separately. Model and `mmproj` weights remain separate downloads.

## Install from a tap

Use this repository as a custom Homebrew tap:

```bash
brew tap ZPVIP/r2t2 https://github.com/ZPVIP/R2T2-ws.git
brew install --HEAD ZPVIP/r2t2/r2t2-ws
```

The Formula is HEAD-only until the project publishes a versioned source archive. It reads the source from the default branch of `ZPVIP/R2T2-ws`.

Run these commands after installation:

```bash
r2t2-ws --version
r2t2-ws --probe
```

The installed process exposes both supported WebSocket interfaces:

- Use `ws://127.0.0.1:8272/asr_stream_api_v1` for clients compatible with the official Confucius4-R2T2 `ws_server.py` interface.
- Use `ws://127.0.0.1:8272/asr` for Tiginal and clients compatible with the public R2T2 web demo.

When authentication is enabled, copy the exact URLs printed by `r2t2-ws`. The native URL uses `token`. The RStream-compatible URL uses `t`.

## Test the Formula from this repository

Push the current changes to the default branch before you run this command. Homebrew clones the `head` URL instead of building the working tree.

```bash
brew install --HEAD ./Formula/r2t2-ws.rb
brew test r2t2-ws
```

To reinstall after a source change, run:

```bash
brew reinstall --HEAD ./Formula/r2t2-ws.rb
```

## Install model files

Download one main model and one compatible projector from the official model repository. Keep the files outside the Homebrew prefix so that an upgrade does not remove them.

```bash
r2t2-ws \
  --model /path/to/Confucius4-R2T2-Q4_K_M.gguf \
  --mmproj /path/to/mmproj-Confucius4-R2T2-Q8_0.gguf \
  --auth auto \
  --port 0
```

## Publish a stable Formula

The maintainer must complete these steps for a stable installation:

1. Create a version tag and a GitHub source archive.
2. Add `url`, `sha256`, and `version` to `Formula/r2t2-ws.rb`.
3. Keep the updated Formula on the default branch.
4. Run `brew audit --strict ZPVIP/r2t2/r2t2-ws` and `brew test ZPVIP/r2t2/r2t2-ws`.
5. Build bottles on supported macOS runners if prebuilt packages are required.

The current Formula compiles on the target Mac. It does not provide bottles.
