import asyncio
from datetime import datetime

from models.udp_config import UdpConfig
from models.xai_config import XaiConfig
from modules.bridget_logger import BridgeLogger
from modules.jitter_buffer import JitterBuffer
from modules.output_waver_wrriter import OutputWaveWriter
from modules.udp_message_sender import UdpMessageSender
from modules.udp_server import UdpServer
from modules.xi_realtime_client import XaiRealtimeClient


class UdpToXaiBridge:
    """Coordinates UDP input, packet ordering, and xAI realtime audio exchange."""

    def __init__(self) -> None:
        now = datetime.now()
        timestamp = now.strftime("%Y-%m-%d_%H-%M-%S")

        self.udp_config = UdpConfig()
        self.xai_config = XaiConfig()

        self.log_file = f"bridge_log_{timestamp}.txt"
        self.wav_out_file = f"audio_out_{timestamp}.wav"

        self.logger = BridgeLogger(self.log_file)
        self.wav_writer = OutputWaveWriter(
            self.wav_out_file,
            self.udp_config.output_sample_rate,
        )

        self.audio_queue: asyncio.Queue = asyncio.Queue()
        self.commit_queue: asyncio.Queue = asyncio.Queue()
        self.output_audio_done_event = asyncio.Event()
        self.output_audio_done_event.set()

        self.output_turn_finished = False
        
        self.jitter_buffer = JitterBuffer(self.udp_config, self.logger)
        self.udp_message_sender = UdpMessageSender(self.logger)

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
            self.wav_writer,
            self.udp_message_sender,
            self.udp_server,
        )

        self.running = True

    async def websocket_sender(self) -> None:
        """
        Forward either audio chunks or commit commands to xAI.

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

    async def udp_audio_sender(self) -> None:
        while True:
            pcm_data = await self.output_audio_queue.get()
            if pcm_data is None:
                break

            await self.udp_message_sender.send_audio_chunked(
                pcm_data,
                self.udp_server.remote_addr,
            )

    async def run(self) -> None:
        """Start the bridge and keep all async tasks running together."""
        try:
            await self.xai_client.connect()

            self.logger.log(f"Output WAV file: {self.wav_out_file}")
            self.logger.log(f"Log file: {self.log_file}")
            self.logger.log(
                (
                    "Bridge config | "
                    f"input_rate={self.udp_config.input_sample_rate} | "
                    f"output_rate={self.udp_config.output_sample_rate} | "
                    f"samples_per_packet={self.udp_config.samples_per_packet} | "
                    f"packet_audio_size={self.udp_config.packet_audio_size}"
                ),
                "CONFIG",
            )

            await asyncio.gather(
                self.websocket_sender(),
                self.xai_client.receive_events(),
                self.xai_client.udp_audio_sender(),
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

            try:
                self.udp_message_sender.close()
            except Exception:
                pass

            try:
                self.wav_writer.close()
            except Exception:
                pass

            self.logger.log("=== END OF TRANSMISSION ===", "SUMMARY")
            self.logger.log(f"Output saved to: {self.wav_out_file}", "SUMMARY")
            self.logger.log(
                f"Total packets received: {self.jitter_buffer.total_received}",
                "SUMMARY",
            )
            self.logger.log(
                f"Total packets lost: {self.jitter_buffer.total_lost} "
                f"({self.jitter_buffer.loss_rate():.2f}%)",
                "SUMMARY",
            )