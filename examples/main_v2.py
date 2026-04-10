import asyncio
import base64
import json
import os
import socket
import struct
from dataclasses import dataclass
from datetime import datetime
from typing import Dict, Optional, Tuple

import websockets


@dataclass(frozen=True)
class UdpConfig:
    """Configuration values related to UDP audio transport."""

    server_port: int = 5000
    buffer_size: int = 4096

    input_sample_rate: int = 16000
    input_samples_per_packet: int = 160

    output_sample_rate: int = 24000
    output_samples_per_packet: int = 240

    metadata_size: int = 4
    jitter_ms: int = 250
    max_buffer: int = 800
    max_missing_before_loss: int = 5

    @property
    def input_packet_audio_size(self) -> int:
        """Return input PCM payload size in bytes for each UDP packet."""
        return self.input_samples_per_packet * 2  # 16-bit mono PCM

    @property
    def output_packet_audio_size(self) -> int:
        """Return output PCM payload size in bytes for each UDP packet."""
        return self.output_samples_per_packet * 2  # 16-bit mono PCM

    @property
    def input_packet_duration_ms(self) -> float:
        """Return input packet duration in milliseconds."""
        return (self.input_samples_per_packet / self.input_sample_rate) * 1000

    @property
    def output_packet_duration_ms(self) -> float:
        """Return output packet duration in milliseconds."""
        return (self.output_samples_per_packet / self.output_sample_rate) * 1000

    @property
    def jitter_size(self) -> int:
        """Return the number of packets required for the input jitter buffer."""
        return max(10, int(self.jitter_ms / self.input_packet_duration_ms))


@dataclass(frozen=True)
class XaiConfig:
    """Configuration values for the xAI realtime connection."""

    realtime_url: str = "wss://api.x.ai/v1/realtime"
    voice: str = "eve"
    default_api_key: str = (
        "xai-YFlh8xKDPTU7yjRZWONmfxEfDIiWAsADx7Dzzvx0u6UmV0FlpxQQWv1ZtRzG63Z7zpHSMrM7v7k1vbIf"
    )
    agent_prompt: str = """
You are WALL-E, a helpful voice assistant who receives audio from the user, transcribes it, and replies with audio. You can also use tools such as web_search and x_search to find information when needed.

Your personality should feel like WALL-E, the lovable and curious robot from the movie: warm, gentle, playful, and endearing. Be concise, friendly, and supportive. Always do your best to help the user clearly and efficiently.

When responding, keep your tone simple and natural. Avoid being overly verbose. Sound expressive and kind, while still being useful and accurate.
"""


class BridgeLogger:
    """Simple logger that writes messages both to stdout and to a log file."""

    def __init__(self, log_file: str) -> None:
        self.log_file = log_file

    def log(self, message: str, level: str = "INFO") -> None:
        """Write a formatted log line to stdout and to the log file."""
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        line = f"[{timestamp}] [{level}] {message}"
        print(line)

        try:
            with open(self.log_file, "a", encoding="utf-8") as file:
                file.write(line + "\n")
        except Exception as exc:
            print(f"Error writing log file: {exc}")


class AssistantUdpStreamer:
    """Streams assistant output audio back to the ESP32 over UDP."""

    def __init__(self, udp_config: UdpConfig, logger: BridgeLogger) -> None:
        self.udp_config = udp_config
        self.logger = logger
        self.sequence: int = 0
        self.pending = bytearray()
        self.total_packets_sent: int = 0

    def reset(self) -> None:
        """Reset packet sequence and pending buffered audio for a new response."""
        self.sequence = 0
        self.pending.clear()
        self.total_packets_sent = 0

    def send_pcm_chunk(
        self,
        sock: Optional[socket.socket],
        remote_addr: Optional[Tuple[str, int]],
        pcm_data: bytes,
    ) -> None:
        """
        Buffer assistant PCM16 mono audio and send it in fixed-size UDP packets.

        Packet layout:
        - 4 bytes: little-endian uint32 sequence number
        - 480 bytes: PCM16 mono audio (240 samples = 10 ms @ 24 kHz)
        """
        if sock is None or remote_addr is None or not pcm_data:
            return

        self.pending.extend(pcm_data)
        packet_size = self.udp_config.output_packet_audio_size

        while len(self.pending) >= packet_size:
            payload = bytes(self.pending[:packet_size])
            del self.pending[:packet_size]

            packet = struct.pack("<I", self.sequence) + payload
            sock.sendto(packet, remote_addr)

            self.sequence += 1
            self.total_packets_sent += 1

    def flush(
        self,
        sock: Optional[socket.socket],
        remote_addr: Optional[Tuple[str, int]],
    ) -> None:
        """Flush remaining output audio, padding the final packet with silence."""
        if not self.pending:
            return

        if sock is None or remote_addr is None:
            self.pending.clear()
            return

        packet_size = self.udp_config.output_packet_audio_size

        if len(self.pending) < packet_size:
            self.pending.extend(b"\x00" * (packet_size - len(self.pending)))

        payload = bytes(self.pending[:packet_size])
        self.pending.clear()

        packet = struct.pack("<I", self.sequence) + payload
        sock.sendto(packet, remote_addr)

        self.sequence += 1
        self.total_packets_sent += 1

    def send_done(
        self,
        sock: Optional[socket.socket],
        remote_addr: Optional[Tuple[str, int]],
    ) -> None:
        """Notify the ESP32 that assistant playback is complete."""
        if sock is None or remote_addr is None:
            return

        sock.sendto(b"PLAYBACK_DONE", remote_addr)
        self.logger.log(
            f"Assistant playback done sent | packets={self.total_packets_sent}",
            "AUDIO",
        )


class JitterBuffer:
    """
    Stores incoming UDP input-audio packets and emits them in sequence order.

    Missing packets are replaced with silence after a configurable number
    of failed attempts to wait for the expected sequence number.
    """

    def __init__(self, config: UdpConfig, logger: BridgeLogger) -> None:
        self.config = config
        self.logger = logger

        self.buffer: Dict[int, bytes] = {}
        self.expected_seq: Optional[int] = None
        self.last_packet_size: int = config.input_packet_audio_size
        self.total_received: int = 0
        self.total_lost: int = 0
        self.missing_counter: int = 0
        self.input_closed: bool = False
        self.turn_index: int = 0

    def reset_for_cancel(self) -> None:
        """Clear all buffered state after a cancel event."""
        self.input_closed = True
        self.buffer.clear()
        self.expected_seq = None
        self.missing_counter = 0

    def open_new_turn_if_needed(self, seq: int) -> None:
        """Start a new input turn if the previous one was already closed."""
        if self.input_closed:
            self.turn_index += 1
            self.buffer.clear()
            self.expected_seq = seq
            self.missing_counter = 0
            self.input_closed = False
            self.logger.log(
                f"New audio turn #{self.turn_index} -> initial seq={seq}",
                "TURN",
            )

    def register_packet(self, seq: int, audio: bytes) -> None:
        """Store an incoming packet in the jitter buffer."""
        self.last_packet_size = len(audio)
        self.total_received += 1

        if self.expected_seq is None:
            self.expected_seq = seq
            self.logger.log(f"First packet received -> seq={seq}", "INIT")

        self.buffer[seq] = audio

    def process_ordered_audio(self, audio_queue: asyncio.Queue) -> None:
        """
        Push in-order input audio packets into the async audio queue.

        If the expected packet does not arrive after several retries,
        silence is inserted to preserve timing continuity.
        """
        if self.input_closed:
            return

        processed_any = False
        processed_count = 0

        while self.expected_seq in self.buffer and processed_count < 50:
            pcm = self.buffer.pop(self.expected_seq)
            audio_queue.put_nowait(pcm)
            self.expected_seq += 1
            self.missing_counter = 0
            processed_count += 1
            processed_any = True

        if not processed_any and self.expected_seq is not None and len(self.buffer) > 0:
            self.missing_counter += 1

            if self.missing_counter >= self.config.max_missing_before_loss:
                silence = b"\x00" * self.last_packet_size
                self.logger.log(
                    f"seq {self.expected_seq} lost -> filled with silence",
                    "LOSS",
                )
                audio_queue.put_nowait(silence)
                self.expected_seq += 1
                self.missing_counter = 0
                self.total_lost += 1

        if len(self.buffer) > self.config.max_buffer and self.expected_seq is not None:
            for old_seq in list(self.buffer.keys()):
                if old_seq < self.expected_seq - 10:
                    del self.buffer[old_seq]

    def close_input_turn(self, audio_queue: asyncio.Queue) -> None:
        """
        Flush any remaining in-order packets and mark the current turn as closed.

        Out-of-order packets still left in the buffer are discarded.
        """
        if self.expected_seq is None:
            self.input_closed = True
            self.missing_counter = 0
            self.buffer.clear()
            self.logger.log("Input turn closed with no pending audio", "TURN")
            return

        flushed = 0
        while self.expected_seq in self.buffer:
            pcm = self.buffer.pop(self.expected_seq)
            audio_queue.put_nowait(pcm)
            self.expected_seq += 1
            flushed += 1

        dropped = len(self.buffer)
        self.buffer.clear()

        self.input_closed = True
        self.expected_seq = None
        self.missing_counter = 0

        self.logger.log(
            f"Input turn closed | flushed={flushed} | dropped_out_of_order={dropped}",
            "TURN",
        )

    def loss_rate(self) -> float:
        """Return the packet loss rate percentage."""
        if self.total_received == 0:
            return 0.0
        return (self.total_lost / self.total_received) * 100


class UdpServer:
    """Receives UDP packets and routes them into the input jitter buffer / control flow."""

    def __init__(
        self,
        config: UdpConfig,
        logger: BridgeLogger,
        jitter_buffer: JitterBuffer,
        audio_queue: asyncio.Queue,
        commit_queue: asyncio.Queue,
    ) -> None:
        self.config = config
        self.logger = logger
        self.jitter_buffer = jitter_buffer
        self.audio_queue = audio_queue
        self.commit_queue = commit_queue

        self.sock: Optional[socket.socket] = None
        self.running: bool = True
        self.remote_addr: Optional[Tuple[str, int]] = None

    async def run(self) -> None:
        """Main UDP receive loop."""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.bind(("", self.config.server_port))
        self.sock.setblocking(True)

        self.logger.log("UDP server started", "START")
        self.logger.log(f"UDP port: {self.config.server_port}")
        self.logger.log(
            f"Input jitter buffer: {self.config.jitter_size} packets "
            f"(~{self.config.jitter_size * self.config.input_packet_duration_ms:.0f} ms)"
        )

        loop = asyncio.get_running_loop()

        while self.running:
            data, addr = await loop.run_in_executor(
                None,
                self.sock.recvfrom,
                self.config.buffer_size,
            )
            self.remote_addr = addr

            if data == b"PING":
                self.sock.sendto(b"PONG", addr)
                continue

            if data == b"COMMIT":
                self.logger.log("UDP COMMIT signal received", "TURN")
                self.jitter_buffer.close_input_turn(self.audio_queue)
                await self.commit_queue.put("commit")
                continue

            if data == b"CANCEL":
                self.logger.log("UDP CANCEL signal received", "TURN")
                self.jitter_buffer.reset_for_cancel()
                await self.commit_queue.put("cancel")
                continue

            if data == b"STOP":
                self.logger.log("UDP STOP signal received", "STOP")
                self.running = False
                break

            if data.startswith(b"HELLO_FROM_ESP32"):
                self.logger.log(
                    f"UDP message: {data.decode(errors='ignore')}",
                    "INFO",
                )
                continue

            if len(data) < self.config.metadata_size:
                self.logger.log(f"Packet too short: {len(data)} bytes", "WARN")
                continue

            seq = struct.unpack("<I", data[: self.config.metadata_size])[0]
            audio = data[self.config.metadata_size :]

            if len(audio) != self.config.input_packet_audio_size:
                self.logger.log(
                    f"Invalid input packet: {len(audio)} bytes "
                    f"(expected {self.config.input_packet_audio_size})",
                    "WARN",
                )
                continue

            self.jitter_buffer.open_new_turn_if_needed(seq)
            self.jitter_buffer.register_packet(seq, audio)
            self.jitter_buffer.process_ordered_audio(self.audio_queue)

            if self.jitter_buffer.total_received % 300 == 0:
                self.logger.log(
                    f"seq={self.jitter_buffer.expected_seq} | "
                    f"buf={len(self.jitter_buffer.buffer)} | "
                    f"rec={self.jitter_buffer.total_received} | "
                    f"lost={self.jitter_buffer.total_lost} "
                    f"({self.jitter_buffer.loss_rate():.2f}%)",
                    "STAT",
                )

    def close(self) -> None:
        """Close the UDP socket if it exists."""
        if self.sock:
            self.sock.close()


class XaiRealtimeClient:
    """Manages the realtime WebSocket connection to xAI."""

    def __init__(
        self,
        config: XaiConfig,
        udp_config: UdpConfig,
        logger: BridgeLogger,
        udp_server: UdpServer,
        assistant_streamer: AssistantUdpStreamer,
    ) -> None:
        self.config = config
        self.udp_config = udp_config
        self.logger = logger
        self.udp_server = udp_server
        self.assistant_streamer = assistant_streamer
        self.ws = None
        self.response_active = False

    async def connect(self) -> None:
        """Open the WebSocket connection and initialize the xAI session."""
        api_key = os.environ.get("XAI_API_KEY", self.config.default_api_key)
        headers = {"Authorization": f"Bearer {api_key}"}

        self.ws = await websockets.connect(
            self.config.realtime_url,
            additional_headers=headers,
        )

        await self.ws.send(
            json.dumps(
                {
                    "type": "session.update",
                    "session": {
                        "voice": self.config.voice,
                        "instructions": self.config.agent_prompt,
                        "turn_detection": None,
                        "tools": [
                            {"type": "web_search"},
                            {"type": "x_search"},
                        ],
                        "input_audio_transcription": {
                            "model": "grok-2-audio"
                        },
                        "audio": {
                            "input": {
                                "format": {
                                    "type": "audio/pcm",
                                    "rate": self.udp_config.input_sample_rate,
                                }
                            },
                            "output": {
                                "format": {
                                    "type": "audio/pcm",
                                    "rate": self.udp_config.output_sample_rate,
                                }
                            },
                        },
                    },
                }
            )
        )

        self.logger.log("Connected to xAI realtime", "START")

    async def send_audio_append(self, pcm_data: bytes) -> None:
        """Send a chunk of PCM input audio to the xAI input audio buffer."""
        await self.ws.send(
            json.dumps(
                {
                    "type": "input_audio_buffer.append",
                    "audio": base64.b64encode(pcm_data).decode("utf-8"),
                }
            )
        )

    async def send_commit(self) -> None:
        """Commit the current input audio buffer and request a response."""
        self.assistant_streamer.reset()
        self.response_active = True

        await self.ws.send(json.dumps({"type": "input_audio_buffer.commit"}))
        await self.ws.send(json.dumps({"type": "response.create"}))
        self.logger.log("COMMIT sent to xAI", "TURN")

    async def close(self) -> None:
        """Close the WebSocket connection if it exists."""
        if self.ws:
            await self.ws.close()

    async def receive_events(self) -> None:
        """Receive and process events coming from the xAI realtime API."""
        try:
            async for raw_event in self.ws:
                event = json.loads(raw_event)
                event_type = event.get("type")

                if event_type == "session.created":
                    session_id = event.get("session", {}).get("id")
                    self.logger.log(f"Session created: {session_id}", "XAI")

                elif event_type == "session.updated":
                    self.logger.log("Session updated", "XAI")

                elif event_type == "input_audio_buffer.committed":
                    self.logger.log("xAI confirmed commit", "XAI")

                elif event_type == "conversation.item.input_audio_transcription.completed":
                    transcript = event.get("transcript", "")
                    self.logger.log(f"User transcription: {transcript}", "USER")

                elif event_type == "response.created":
                    self.logger.log("Assistant response created", "ASSISTANT")

                elif event_type == "response.output_audio.delta":
                    pcm_data = base64.b64decode(event["delta"])
                    self.assistant_streamer.send_pcm_chunk(
                        self.udp_server.sock,
                        self.udp_server.remote_addr,
                        pcm_data,
                    )

                elif event_type == "response.output_audio_transcript.delta":
                    print(event["delta"], end="", flush=True)

                elif event_type == "response.output_audio.done":
                    self.assistant_streamer.flush(
                        self.udp_server.sock,
                        self.udp_server.remote_addr,
                    )
                    self.assistant_streamer.send_done(
                        self.udp_server.sock,
                        self.udp_server.remote_addr,
                    )
                    print()
                    self.logger.log("Response audio completed", "ASSISTANT")

                elif event_type == "response.done":
                    usage = event.get("usage", {})
                    self.response_active = False
                    self.logger.log(
                        f"Response completed | tokens={usage.get('total_tokens')}",
                        "XAI",
                    )

                elif event_type == "error":
                    self.response_active = False
                    self.logger.log(
                        f"xAI error: {json.dumps(event, ensure_ascii=False)}",
                        "ERROR",
                    )

        except Exception as exc:
            self.logger.log(f"websocket receiver error: {exc}", "ERROR")


class UdpToXaiBridge:
    """Coordinates UDP input, packet ordering, and xAI realtime audio exchange."""

    def __init__(self) -> None:
        now = datetime.now()
        timestamp = now.strftime("%Y-%m-%d_%H-%M-%S")

        self.udp_config = UdpConfig()
        self.xai_config = XaiConfig()

        self.log_file = f"bridge_log_{timestamp}.txt"
        self.logger = BridgeLogger(self.log_file)

        self.audio_queue: asyncio.Queue = asyncio.Queue()
        self.commit_queue: asyncio.Queue = asyncio.Queue()

        self.jitter_buffer = JitterBuffer(self.udp_config, self.logger)
        self.assistant_streamer = AssistantUdpStreamer(
            self.udp_config,
            self.logger,
        )

        self.udp_server = UdpServer(
            self.udp_config,
            self.logger,
            self.jitter_buffer,
            self.audio_queue,
            self.commit_queue,
        )

        self.xai_client = XaiRealtimeClient(
            self.xai_config,
            self.udp_config,
            self.logger,
            self.udp_server,
            self.assistant_streamer,
        )

        self.running = True

    async def websocket_sender(self) -> None:
        """
        Forward either input-audio chunks or commit commands to xAI.

        Audio packets are appended to the input buffer.
        Commit commands finalize the current user turn and request a response.
        """
        while self.running:
            audio_task = asyncio.create_task(self.audio_queue.get())
            commit_task = asyncio.create_task(self.commit_queue.get())

            done, pending = await asyncio.wait(
                {audio_task, commit_task},
                return_when=asyncio.FIRST_COMPLETED,
            )

            for task in pending:
                task.cancel()

            if commit_task in done:
                try:
                    command = commit_task.result()
                except asyncio.CancelledError:
                    continue
                except Exception as exc:
                    self.logger.log(f"commit task error: {exc}", "ERROR")
                    continue

                if command == "commit":
                    await self.xai_client.send_commit()
                elif command == "cancel":
                    self.logger.log("CANCEL requested", "TURN")
                continue

            if audio_task in done:
                try:
                    pcm_data = audio_task.result()
                except asyncio.CancelledError:
                    continue
                except Exception as exc:
                    self.logger.log(f"audio task error: {exc}", "ERROR")
                    continue

                if pcm_data is None:
                    break

                await self.xai_client.send_audio_append(pcm_data)

    async def run(self) -> None:
        """Start the bridge and keep all async tasks running together."""
        try:
            await self.xai_client.connect()

            self.logger.log(f"Log file: {self.log_file}")
            self.logger.log(
                f"Input audio: PCM16 mono {self.udp_config.input_sample_rate} Hz",
                "INFO",
            )
            self.logger.log(
                f"Input packet audio size: {self.udp_config.input_packet_audio_size} bytes",
                "INFO",
            )
            self.logger.log(
                f"Input packet duration: {self.udp_config.input_packet_duration_ms:.1f} ms",
                "INFO",
            )
            self.logger.log(
                f"Output audio: PCM16 mono {self.udp_config.output_sample_rate} Hz",
                "INFO",
            )
            self.logger.log(
                f"Output packet audio size: {self.udp_config.output_packet_audio_size} bytes",
                "INFO",
            )
            self.logger.log(
                f"Output packet duration: {self.udp_config.output_packet_duration_ms:.1f} ms",
                "INFO",
            )

            await asyncio.gather(
                self.websocket_sender(),
                self.xai_client.receive_events(),
                self.udp_server.run(),
            )

        except KeyboardInterrupt:
            self.logger.log("Stopped by user", "STOP")
        except Exception as exc:
            self.logger.log(f"Unexpected error: {exc}", "ERROR")
        finally:
            self.running = False
            self.udp_server.running = False

            try:
                await self.audio_queue.put(None)
            except Exception:
                pass

            try:
                await self.commit_queue.put("cancel")
            except Exception:
                pass

            try:
                await self.xai_client.close()
            except Exception:
                pass

            try:
                self.udp_server.close()
            except Exception:
                pass

            self.logger.log("=== END OF TRANSMISSION ===", "SUMMARY")
            self.logger.log(
                f"Total input packets received: {self.jitter_buffer.total_received}",
                "SUMMARY",
            )
            self.logger.log(
                f"Total input packets lost: {self.jitter_buffer.total_lost} "
                f"({self.jitter_buffer.loss_rate():.2f}%)",
                "SUMMARY",
            )
            self.logger.log(
                f"Assistant output packets sent: {self.assistant_streamer.total_packets_sent}",
                "SUMMARY",
            )


async def main() -> None:
    """Application entry point."""
    bridge = UdpToXaiBridge()
    await bridge.run()


if __name__ == "__main__":
    asyncio.run(main())