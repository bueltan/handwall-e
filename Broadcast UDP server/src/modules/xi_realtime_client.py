import asyncio
import base64
from datetime import datetime
import json
import os

import websockets

from models.udp_config import UdpConfig
from models.xai_config import XaiConfig
from modules.bridget_logger import BridgeLogger
from modules.output_waver_wrriter import OutputWaveWriter
from modules.udp_message_sender import UdpMessageSender
from modules.udp_server import UdpServer


class XaiRealtimeClient:
    """Manages the realtime WebSocket connection to xAI."""

    def __init__(
        self,
        config: XaiConfig,
        udp_config: UdpConfig,
        logger: BridgeLogger,
        wav_writer: OutputWaveWriter,
        udp_message_sender: UdpMessageSender,
        udp_server: UdpServer,
    ) -> None:
        self.config = config
        self.udp_config = udp_config
        self.logger = logger
        self.wav_writer = wav_writer
        self.udp_message_sender = udp_message_sender
        self.udp_server = udp_server

        self.ws = None
        self.current_assistant_text = ""

        self.output_audio_bytes = 0
        self.output_audio_chunks = 0

        self.output_audio_queue: asyncio.Queue[bytes | None] = asyncio.Queue()
        self.output_audio_done_event = asyncio.Event()
        self.output_audio_done_event.set()
        self.output_turn_finished = False

    async def udp_audio_sender(self) -> None:
        """Send assistant audio to the ESP32 without blocking websocket reads."""
        while True:
            pcm_data = await self.output_audio_queue.get()

            if pcm_data is None:
                break

            await self.udp_message_sender.send_audio_chunked(
                pcm_data,
                self.udp_server.remote_addr,
            )

            if self.output_audio_queue.empty() and self.output_turn_finished:
                self.udp_message_sender.send_audio_end(
                    self.udp_server.remote_addr,
                )
                self.output_turn_finished = False
                self.output_audio_done_event.set()

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

        self.logger.log(
            (
                "Connected to xAI realtime | "
                f"input_rate={self.udp_config.input_sample_rate} | "
                f"output_rate={self.udp_config.output_sample_rate}"
            ),
            "START",
        )

    async def send_audio_append(self, pcm_data: bytes) -> None:
        """Send a chunk of PCM audio to the xAI input audio buffer."""
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
        self.current_assistant_text = ""
        self.output_audio_bytes = 0
        self.output_audio_chunks = 0
        self.output_turn_finished = False
        self.output_audio_done_event.clear()

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
                    transcript = event.get("transcript", "").strip()
                    self.logger.log(f"User transcription: {transcript}", "USER")

                elif event_type == "response.output_audio.delta":
                    pcm_data = base64.b64decode(event["delta"])

                    self.output_audio_chunks += 1
                    self.output_audio_bytes += len(pcm_data)

                    if self.output_audio_chunks <= 5 or self.output_audio_chunks % 25 == 0:
                        samples = len(pcm_data) // 2
                        approx_ms = (
                            samples / self.udp_config.output_sample_rate
                        ) * 1000.0
                        self.logger.log(
                            (
                                f"Output audio chunk #{self.output_audio_chunks} | "
                                f"bytes={len(pcm_data)} | samples={samples} | "
                                f"~{approx_ms:.2f} ms"
                            ),
                            "AUDIO",
                        )

                    #self.wav_writer.write(pcm_data)
                    await self.output_audio_queue.put(pcm_data)

                elif event_type == "response.output_audio_transcript.delta":
                    delta_text = event.get("delta", "")
                    self.current_assistant_text += delta_text
                    print(delta_text, end="", flush=True)

                elif event_type == "response.output_audio.done":
                    print()

                    total_samples = self.output_audio_bytes // 2
                    total_ms = (
                        total_samples / self.udp_config.output_sample_rate
                    ) * 1000.0

                    self.logger.log(
                        (
                            "Response audio completed | "
                            f"chunks={self.output_audio_chunks} | "
                            f"bytes={self.output_audio_bytes} | "
                            f"samples={total_samples} | "
                            f"~{total_ms:.2f} ms"
                        ),
                        "ASSISTANT",
                    )

                    # Do not send __END__ here.
                    # The udp_audio_sender task will send it only after the queue is drained.
                    self.output_turn_finished = True

                    if self.output_audio_queue.empty():
                        self.udp_message_sender.send_audio_end(
                            self.udp_server.remote_addr,
                        )
                        self.output_turn_finished = False
                        self.output_audio_done_event.set()

                elif event_type == "response.done":
                    await self.output_audio_done_event.wait()

                    usage = event.get("usage", {})
                    self.logger.log(
                        f"Response completed | tokens={usage.get('total_tokens')}",
                        "XAI",
                    )

                    assistant_text = self.current_assistant_text.strip()
                    if assistant_text:
                        self.logger.log(f"Assistant response: {assistant_text}")
                        self.logger.log(
                            f"UDP target addr: {self.udp_server.remote_addr}",
                            "UDP",
                        )
                        self.udp_message_sender.send_json(
                            {
                                "type": "assistant_response",
                                "value": assistant_text,
                                "final": True,
                                "timestamp": datetime.now().isoformat(
                                    timespec="seconds"
                                ),
                            },
                            self.udp_server.remote_addr,
                        )

                elif event_type == "error":
                    self.logger.log(
                        f"xAI error: {json.dumps(event, ensure_ascii=False)}",
                        "ERROR",
                    )

        except Exception as exc:
            self.logger.log(f"websocket receiver error: {exc}", "ERROR")