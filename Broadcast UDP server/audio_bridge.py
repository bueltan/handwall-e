import asyncio
import socket
import struct
import json
import os
import base64
import websockets
import time
from datetime import datetime

# ====================== CONFIG ======================
SERVER_PORT = 5000
BUFFER_SIZE = 4096

SAMPLE_RATE = 16000
SAMPLES_PER_PACKET = 160
PACKET_AUDIO_SIZE = SAMPLES_PER_PACKET * 2
METADATA_SIZE = 4

MAX_MISSING = 5
SILENCE_TIMEOUT = 0.6
REAL_SILENCE_COMMIT = 3.0  # 👈 commit automático si silencio > Xs

SILENCE_CHUNK = b'\x00' * PACKET_AUDIO_SIZE

-

# ====================== LOG ======================
def log(msg):
    print(f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] {msg}")

# ====================== UDP RECEIVER ======================
class UdpReceiver:
    def __init__(self, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", port))
        self.sock.setblocking(False)

    async def receive(self):
        loop = asyncio.get_event_loop()
        try:
            data, addr = await loop.sock_recvfrom(self.sock, BUFFER_SIZE)
            return data, addr
        except:
            return None, None

# ====================== JITTER BUFFER ======================
class JitterBuffer:
    def __init__(self):
        self.buffer = {}
        self.expected_seq = None
        self.missing_counter = 0

    def push(self, seq, chunk):
        self.buffer[seq] = chunk

    def process(self):
        output = []
        if self.expected_seq is None:
            return output

        while True:
            if self.expected_seq in self.buffer:
                output.append(self.buffer.pop(self.expected_seq))
                self.expected_seq += 1
                self.missing_counter = 0
            else:
                self.missing_counter += 1
                if self.missing_counter >= MAX_MISSING:
                    log(f"⚠️ Loss → silencio seq={self.expected_seq}")
                    output.append(SILENCE_CHUNK)
                    self.expected_seq += 1
                    self.missing_counter = 0
                break
        return output

# ====================== GROK CLIENT ======================
class GrokClient:
    def __init__(self):
        self.ws = None
        self.last_audio_time = time.time()
        self.silence_start = None

    async def connect(self):
        uri = "wss://api.x.ai/v1/realtime"
        headers = {"Authorization": f"Bearer {XAI_API_KEY}"}

        self.ws = await websockets.connect(uri, additional_headers=headers)

        await self.ws.send(json.dumps({
            "type": "session.update",
            "session": {
                "voice": "Eve",
                "instructions": "Transcribe el audio y responde SIEMPRE en JSON: {\"text\":\"...\",\"confidence\":0.0}",
                "input_audio_format": "pcm16",
                "turn_detection": {"type": "server_vad"}
            }
        }))
        log("✅ Conectado a Grok")

    async def send_audio(self, chunk, is_silence=False):
        await self.ws.send(json.dumps({
            "type": "input_audio_buffer.append",
            "audio": base64.b64encode(chunk).decode()
        }))

        now = time.time()
        if not is_silence:
            self.last_audio_time = now
            self.silence_start = None
        else:
            if self.silence_start is None:
                self.silence_start = now

    async def commit_audio(self):
        log("💾 COMMIT manual")
        await self.ws.send(json.dumps({"type": "input_audio_buffer.commit"}))
        await self.ws.send(json.dumps({"type": "response.create"}))
        self.silence_start = None
        self.last_audio_time = time.time()

    async def discard_audio(self):
        log("🗑️ CANCEL manual → descartando audio")
        await self.ws.send(json.dumps({"type": "input_audio_buffer.reset"}))
        self.silence_start = None
        self.last_audio_time = time.time()

    async def handle_real_silence(self):
        if self.silence_start is None:
            return
        now = time.time()
        if now - self.silence_start >= REAL_SILENCE_COMMIT:
            log(f"🛑 Silencio real {now - self.silence_start:.2f}s → COMMIT")
            await self.commit_audio()

    async def listen(self):
        async for message in self.ws:
            try:
                event = json.loads(message)
            except:
                continue

            etype = event.get("type")
            if etype == "response.output_text.delta":
                print(event.get("delta", ""), end="", flush=True)
            elif etype == "response.completed":
                try:
                    text = event["response"]["output"][0]["content"][0]["text"]
                    parsed = json.loads(text)
                    print("\n📦 JSON:", parsed)
                except:
                    print("\n⚠️ JSON inválido:", event)
            elif etype == "input_audio_buffer.speech_started":
                print("\n🎤 Escuchando...")
            elif etype == "error":
                print("\n❌ ERROR:", event)

# ====================== AUDIO PIPELINE ======================
class AudioPipeline:
    def __init__(self):
        self.udp = UdpReceiver(SERVER_PORT)
        self.jitter = JitterBuffer()
        self.grok = GrokClient()

    async def run(self):
        await self.grok.connect()
        asyncio.create_task(self.grok.listen())
        log(f"🚀 Escuchando UDP en {SERVER_PORT}")

        while True:
            data, addr = await self.udp.receive()

            # ====================== NO DATA ======================
            if not data:
                await self.grok.send_audio(SILENCE_CHUNK, is_silence=True)
                await self.grok.handle_real_silence()
                await asyncio.sleep(0.02)
                continue

            # ====================== COMANDOS ESP32 ======================
            try:
                cmd = data.decode().strip().upper()
                if cmd == "COMMIT":
                    await self.grok.commit_audio()
                    continue
                elif cmd == "CANCEL":
                    await self.grok.discard_audio()
                    continue
                elif cmd == "PING":
                    self.udp.sock.sendto(b"PONG", addr)
                    continue
            except:
                pass

            # ====================== AUDIO ======================
            if len(data) < METADATA_SIZE:
                continue

            seq = struct.unpack("<I", data[:METADATA_SIZE])[0]
            chunk = data[METADATA_SIZE:]
            if len(chunk) != PACKET_AUDIO_SIZE:
                continue

            # Inicializar secuencia
            if self.jitter.expected_seq is None:
                self.jitter.expected_seq = seq
                log(f"INIT seq={seq}")
                continue

            self.jitter.push(seq, chunk)
            for ordered_chunk in self.jitter.process():
                await self.grok.send_audio(ordered_chunk, is_silence=False)

# ====================== MAIN ======================
async def main():
    pipeline = AudioPipeline()
    await pipeline.run()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nCerrando...")