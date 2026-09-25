# Third-party notices

This project links these source dependencies:

| Dependency | Revision | License | Purpose |
|---|---|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | `ad6c66839af3c5646fba8c6c2e2087a1e4e38948` | MIT | llama inference, ggml, Metal, CUDA, Vulkan, and mtmd audio |
| [IXWebSocket](https://github.com/machinezone/IXWebSocket) | `64fae7676bd8fe31f7cb4bcde7a6841892dad65e` (`v12.0.1`) | BSD-3-Clause | Cross-platform WebSocket server |
| [JSON for Modern C++](https://github.com/nlohmann/json) | `55f93686c01528224f448c19128836e7df245f72` (`v3.12.0`) | MIT | JSON protocol encoding and parsing |
| [Confucius4-R2T2](https://github.com/netease-youdao/Confucius4-R2T2) | inspected at `26d55a54ce5670cff9947a167d8ed95d569fd4d9` | Apache-2.0 | Behavioral reference and adapted native and streaming logic |

Adapted source files retain NetEase Youdao copyright and SPDX notices.

The Confucius4-R2T2 model weights use a separate NetEase model license. This repository does not contain or redistribute model weights. Review the model repository license before packaging or distributing the weights.

The project source is distributed under [GPL-3.0](LICENSE). Include the dependency license texts and required notices in packaged applications.
