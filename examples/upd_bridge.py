# UDP -> xAI Voice Agent bridge
# pip install websockets
# export XAI_API_KEY="xai-..."

import asyncio
import base64
import json
import os
import socket
import struct
import wave
from datetime import datetime

import websockets

# ====================== CONFIG UDP ======================
SERVER_PORT = 5000
BUFFER_SIZE = 4096

SAMPLE_RATE = 16000
SAMPLES_PER_PACKET = 160
PACKET_AUDIO_SIZE = SAMPLES_PER_PACKET * 2   # 320 bytes PCM16 mono
METADATA_SIZE = 4
PACKET_DURATION_MS = (SAMPLES_PER_PACKET / SAMPLE_RATE) * 1000  # 10 ms

JITTER_MS = 250
JITTER_SIZE = max(10, int(JITTER_MS / PACKET_DURATION_MS))
MAX_BUFFER = 800
MAX_MISSING_BEFORE_LOSS = 5

# ====================== CONFIG xAI ======================
REALTIME_URL = "wss://api.x.ai/v1/realtime"
VOICE = "eve"

AGENT_PROMPT = """
You are a helpful voice assistant.
Reply naturally and concisely.
"""

# ====================== ARCHIVOS ======================
now = datetime.now()
timestamp = now.strftime("%Y-%m-%d_%H-%M-%S")

LOG_FILE = f"bridge_log_{timestamp}.txt"
WAV_IN_FILE = f"audio_in_{timestamp}.wav"
WAV_OUT_FILE = f"audio_out_{timestamp}.wav"

# ====================== LOG ======================
def log(message, level="INFO"):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    line = f"[{ts}] [{level}] {message}"
    print(line)
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception as e:
        print(f"Error escribiendo log: {e}")

# ====================== BRIDGE ======================
class UdpToXaiBridge:
    def __init__(self):
        self.ws = None
        self.sock = None

        # estado UDP / jitter
        self.buffer = {}
        self.expected_seq = None
        self.last_packet_size = PACKET_AUDIO_SIZE
        self.total_received = 0
        self.total_lost = 0
        self.missing_counter = 0

        # WAV entrada
        self.wav_in = wave.open(WAV_IN_FILE, "wb")
        self.wav_in.setnchannels(1)
        self.wav_in.setsampwidth(2)
        self.wav_in.setframerate(SAMPLE_RATE)

        # WAV salida
        self.wav_out = wave.open(WAV_OUT_FILE, "wb")
        self.wav_out.setnchannels(1)
        self.wav_out.setsampwidth(2)
        self.wav_out.setframerate(SAMPLE_RATE)

        # cola async para pasar audio ordenado al websocket
        self.audio_queue = asyncio.Queue()
        self.commit_queue = asyncio.Queue()

        self.running = True
        self.remote_addr = None

    async def connect_xai(self):
        headers = {"Authorization": f"Bearer {os.environ.get('XAI_API_KEY', 'xai-YFlh8xKDPTU7yjRZWONmfxEfDIiWAsADx7Dzzvx0u6UmV0FlpxQQWv1ZtRzG63Z7zpHSMrM7v7k1vbIf')}"}

        self.ws = await websockets.connect(
            REALTIME_URL,
            additional_headers=headers
        )

        await self.ws.send(json.dumps({
            "type": "session.update",
            "session": {
                "voice": VOICE,
                "instructions": AGENT_PROMPT,
                "turn_detection": None,
                "tools": [
                    {"type": "web_search"},
                    {"type": "x_search"}
                ],
                "input_audio_transcription": {
                    "model": "grok-2-audio"
                },
                "audio": {
                    "input": {
                        "format": {
                            "type": "audio/pcm",
                            "rate": SAMPLE_RATE
                        }
                    },
                    "output": {
                        "format": {
                            "type": "audio/pcm",
                            "rate": SAMPLE_RATE
                        }
                    }
                }
            }
        }))

        log("Conectado a xAI realtime", "START")

    async def websocket_sender(self):
        while self.running:
            # priorizar commits si llegan
            if not self.commit_queue.empty():
                cmd = await self.commit_queue.get()
                if cmd == "commit":
                    await self.ws.send(json.dumps({"type": "input_audio_buffer.commit"}))
                    await self.ws.send(json.dumps({"type": "response.create"}))
                    log("COMMIT enviado a xAI", "TURN")
                continue

            pcm = await self.audio_queue.get()
            if pcm is None:
                break

            await self.ws.send(json.dumps({
                "type": "input_audio_buffer.append",
                "audio": base64.b64encode(pcm).decode("utf-8")
            }))

    async def websocket_receiver(self):
        try:
            async for raw in self.ws:
                event = json.loads(raw)
                etype = event.get("type")

                if etype == "session.created":
                    sid = event.get("session", {}).get("id")
                    log(f"Session creada: {sid}", "XAI")

                elif etype == "session.updated":
                    log("Session actualizada", "XAI")

                elif etype == "input_audio_buffer.committed":
                    log("xAI confirmó commit", "XAI")

                elif etype == "conversation.item.input_audio_transcription.completed":
                    transcript = event.get("transcript", "")
                    log(f"Transcripción usuario: {transcript}", "USER")

                elif etype == "response.output_audio.delta":
                    pcm = base64.b64decode(event["delta"])
                    self.wav_out.writeframes(pcm)

                elif etype == "response.output_audio_transcript.delta":
                    print(event["delta"], end="", flush=True)

                elif etype == "response.output_audio.done":
                    print()
                    log("Audio de respuesta completado", "ASSISTANT")

                elif etype == "response.done":
                    usage = event.get("usage", {})
                    log(
                        f"Respuesta finalizada | tokens={usage.get('total_tokens')}",
                        "XAI"
                    )

                elif etype == "error":
                    log(f"Error xAI: {json.dumps(event, ensure_ascii=False)}", "ERROR")

        except Exception as e:
            log(f"websocket_receiver error: {e}", "ERROR")

    def process_ordered_audio(self):
        processed_any = False
        processed = 0

        while self.expected_seq in self.buffer and processed < 50:
            pcm = self.buffer.pop(self.expected_seq)
            self.wav_in.writeframes(pcm)
            self.audio_queue.put_nowait(pcm)
            self.expected_seq += 1
            self.missing_counter = 0
            processed += 1
            processed_any = True

        if not processed_any and self.expected_seq is not None and len(self.buffer) > 0:
            self.missing_counter += 1
            if self.missing_counter >= MAX_MISSING_BEFORE_LOSS:
                silence = b"\x00" * self.last_packet_size
                log(f"[LOSS] seq {self.expected_seq} perdido -> relleno con silencio", "LOSS")
                self.wav_in.writeframes(silence)
                self.audio_queue.put_nowait(silence)
                self.expected_seq += 1
                self.missing_counter = 0
                self.total_lost += 1

        if len(self.buffer) > MAX_BUFFER and self.expected_seq is not None:
            for old in list(self.buffer.keys()):
                if old < self.expected_seq - 10:
                    del self.buffer[old]

    def flush_pending_audio(self):
        if self.expected_seq is None:
            return

        flushed = 0
        while self.expected_seq in self.buffer:
            pcm = self.buffer.pop(self.expected_seq)
            self.wav_in.writeframes(pcm)
            self.audio_queue.put_nowait(pcm)
            self.expected_seq += 1
            flushed += 1

        if flushed:
            log(f"Flush antes de commit: {flushed} paquetes", "TURN")

    async def udp_loop(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.bind(("", SERVER_PORT))
        self.sock.setblocking(True)

        log("Servidor UDP iniciado", "START")
        log(f"Puerto UDP: {SERVER_PORT}")
        log(f"Archivo WAV entrada: {WAV_IN_FILE}")
        log(f"Archivo WAV salida: {WAV_OUT_FILE}")
        log(f"Archivo log: {LOG_FILE}")
        log(f"Jitter buffer: {JITTER_SIZE} paquetes (~{JITTER_SIZE * PACKET_DURATION_MS:.0f} ms)")

        loop = asyncio.get_running_loop()

        while self.running:
            data, addr = await loop.run_in_executor(None, self.sock.recvfrom, BUFFER_SIZE)
            self.remote_addr = addr

            if data == b"PING":
                self.sock.sendto(b"PONG", addr)
                continue

            if data == b"COMMIT":
                log("Señal UDP COMMIT recibida", "TURN")
                self.flush_pending_audio()
                await self.commit_queue.put("commit")
                continue

            if data == b"STOP":
                log("Señal UDP STOP recibida", "STOP")
                self.running = False
                break

            if len(data) < METADATA_SIZE:
                continue

            seq = struct.unpack("<I", data[:METADATA_SIZE])[0]
            audio = data[METADATA_SIZE:]

            if len(audio) != PACKET_AUDIO_SIZE:
                log(
                    f"Paquete inválido: {len(audio)} bytes (esperado {PACKET_AUDIO_SIZE})",
                    "WARN"
                )
                continue

            self.buffer[seq] = audio
            self.last_packet_size = len(audio)
            self.total_received += 1

            if self.expected_seq is None:
                self.expected_seq = seq
                log(f"Primer paquete recibido -> seq={seq}", "INIT")
                continue

            self.process_ordered_audio()

            if self.total_received % 300 == 0:
                loss_rate = (self.total_lost / self.total_received * 100) if self.total_received else 0
                log(
                    f"[STAT] seq={self.expected_seq} | buf={len(self.buffer)} | "
                    f"rec={self.total_received} | lost={self.total_lost} ({loss_rate:.2f}%)",
                    "STAT"
                )

    async def run(self):
        try:
            await self.connect_xai()

            await asyncio.gather(
                self.websocket_sender(),
                self.websocket_receiver(),
                self.udp_loop(),
            )

        except KeyboardInterrupt:
            log("Detenido por el usuario", "STOP")
        except Exception as e:
            log(f"Error inesperado: {e}", "ERROR")
        finally:
            self.running = False

            try:
                await self.audio_queue.put(None)
            except Exception:
                pass

            try:
                if self.ws:
                    await self.ws.close()
            except Exception:
                pass

            try:
                if self.sock:
                    self.sock.close()
            except Exception:
                pass

            try:
                self.wav_in.close()
            except Exception:
                pass

            try:
                self.wav_out.close()
            except Exception:
                pass

            final_loss_rate = (self.total_lost / self.total_received * 100) if self.total_received else 0
            log("=== FIN DE TRANSMISIÓN ===", "SUMMARY")
            log(f"Entrada guardada: {WAV_IN_FILE}", "SUMMARY")
            log(f"Salida guardada: {WAV_OUT_FILE}", "SUMMARY")
            log(f"Total paquetes recibidos: {self.total_received}", "SUMMARY")
            log(f"Total paquetes perdidos: {self.total_lost} ({final_loss_rate:.2f}%)", "SUMMARY")


async def main():
    bridge = UdpToXaiBridge()
    await bridge.run()


if __name__ == "__main__":
    asyncio.run(main())