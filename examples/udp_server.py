import socket
import struct

class UdpServer:
    """Receives UDP packets and routes them into the jitter buffer / control flow."""

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
            f"Jitter buffer: {self.config.jitter_size} packets "
            f"(~{self.config.jitter_size * self.config.packet_duration_ms:.0f} ms)"
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

            if len(audio) != self.config.packet_audio_size:
                self.logger.log(
                    f"Invalid packet: {len(audio)} bytes "
                    f"(expected {self.config.packet_audio_size})",
                    "WARN",
                )
                continue

            self.jitter_buffer.open_new_turn_if_needed(seq)
            self.jitter_buffer.register_packet(seq, audio)
            self.jitter_buffer.process_ordered_audio(self.audio_queue)

            if self.jitter_buffer.total_received % 300 == 0:
                self.logger.log(
                    f"[STAT] seq={self.jitter_buffer.expected_seq} | "
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
