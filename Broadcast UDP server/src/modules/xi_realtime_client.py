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
                                    "rate": self.udp_config.sample_rate,
                                }
                            },
                            "output": {
                                "format": {
                                    "type": "audio/pcm",
                                    "rate": self.udp_config.sample_rate,
                                }
                            },
                        },
                    },
                }
            )
        )

        self.logger.log("Connected to xAI realtime", "START")

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
                    self.wav_writer.write(pcm_data)

                elif event_type == "response.output_audio_transcript.delta":
                    delta_text = event.get("delta", "")
                    self.current_assistant_text += delta_text
                    print(delta_text, end="", flush=True)

                elif event_type == "response.output_audio.done":
                    print()
                    self.logger.log("Response audio completed", "ASSISTANT")

                elif event_type == "response.done":
                    usage = event.get("usage", {})
                    self.logger.log(
                        f"Response completed | tokens={usage.get('total_tokens')}",
                        "XAI",
                    )

                    assistant_text = self.current_assistant_text.strip()
                    if assistant_text:
                        self.logger.log(f"Assistant response: {assistant_text}")
                        self.logger.log(f"UDP target addr: {self.udp_server.remote_addr}", "UDP")
                        self.udp_message_sender.send_json(
                            {
                                "type": "assistant_response",
                                "value": assistant_text,
                                "final": True,
                                "timestamp": datetime.now().isoformat(timespec="seconds"),
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
