import { readFile } from "node:fs/promises";
import process from "node:process";

const EOS = "YOUDAO_ONETIME_ASR_STREAM_EOS";

function usage() {
  console.error("Usage: node examples/node-electron-client.mjs <ws-url> <16k-mono-pcm16.wav> [--realtime]");
}

function readWavePcm16(wave) {
  if (wave.length < 12 || wave.toString("ascii", 0, 4) !== "RIFF" || wave.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error("Input is not a RIFF/WAVE file");
  }

  let format;
  let pcm;
  for (let offset = 12; offset + 8 <= wave.length;) {
    const id = wave.toString("ascii", offset, offset + 4);
    const size = wave.readUInt32LE(offset + 4);
    const begin = offset + 8;
    const end = begin + size;
    if (end > wave.length) throw new Error(`Truncated WAVE chunk: ${id}`);
    if (id === "fmt ") {
      format = {
        encoding: wave.readUInt16LE(begin),
        channels: wave.readUInt16LE(begin + 2),
        sampleRate: wave.readUInt32LE(begin + 4),
        bitsPerSample: wave.readUInt16LE(begin + 14),
      };
    } else if (id === "data") {
      pcm = wave.subarray(begin, end);
    }
    offset = end + (size & 1);
  }

  if (!format || !pcm) throw new Error("WAVE file is missing fmt or data");
  if (format.encoding !== 1 || format.channels !== 1 || format.sampleRate !== 16000 || format.bitsPerSample !== 16) {
    throw new Error("WAVE must be PCM, 16 kHz, mono, and 16-bit");
  }
  return pcm;
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function main() {
  const [url, wavePath, option] = process.argv.slice(2);
  if (!url || !wavePath || (option && option !== "--realtime")) {
    usage();
    process.exitCode = 2;
    return;
  }

  const pcm = readWavePcm16(await readFile(wavePath));
  const realtime = option === "--realtime";
  const socket = new WebSocket(url);
  socket.binaryType = "arraybuffer";

  const closed = new Promise((resolve, reject) => {
    let transcript = "";
    let streamingStarted = false;

    socket.addEventListener("open", () => {
      socket.send(JSON.stringify({
        requestId: crypto.randomUUID(),
        channels: 1,
        sample_rate: 16000,
        language: "Chinese",
        use_vad: false,
        mode: "stream_llama",
      }));
    });

    socket.addEventListener("message", async (event) => {
      const message = JSON.parse(typeof event.data === "string" ? event.data : Buffer.from(event.data).toString("utf8"));
      console.log(JSON.stringify(message));
      if (message.status === "error") {
        reject(new Error(`${message.error.code}: ${message.error.message}`));
        return;
      }
      if (message.status === "connected" && !streamingStarted) {
        streamingStarted = true;
        const frameBytes = 5120;
        for (let offset = 0; offset < pcm.length; offset += frameBytes) {
          socket.send(pcm.subarray(offset, Math.min(offset + frameBytes, pcm.length)));
          if (realtime) await delay(160);
        }
        socket.send(EOS);
        return;
      }
      if (message.status === "success") {
        transcript += message.msg.text;
        if (message.msg.reset) console.log(`Final transcript: ${transcript}`);
      }
    });

    socket.addEventListener("error", () => reject(new Error("WebSocket transport error")));
    socket.addEventListener("close", (event) => {
      if (event.code === 1000) resolve(transcript);
      else reject(new Error(`WebSocket closed with code ${event.code}: ${event.reason}`));
    });
  });

  await closed;
}

main().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
