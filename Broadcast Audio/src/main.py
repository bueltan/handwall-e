import asyncio
import wave
from pathlib import Path

HOST = "0.0.0.0"
PORT = 9999
WAV_FILE = "wall-e.wav"
CHUNK_FRAMES = 640


class UDPAudioSender(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport = None
        self.client_tasks = {}

    def connection_made(self, transport):
        self.transport = transport
        print("[SERVER] UDP server ready")

    def datagram_received(self, data, addr):
        message = data.decode(errors="ignore").strip()
        print(f"[SERVER] Client message from {addr}: {message}")

        if message == "START":
            self.restart_stream_for_client(addr)

    def restart_stream_for_client(self, addr):
        old_task = self.client_tasks.get(addr)
        if old_task is not None and not old_task.done():
            print(f"[SERVER] Cancelling previous stream for {addr}")
            old_task.cancel()

        task = asyncio.create_task(self.stream_wav(addr))
        self.client_tasks[addr] = task

    async def stream_wav(self, addr):
        wav_path = Path(WAV_FILE)

        try:
            if not wav_path.exists():
                print(f"[SERVER] File not found: {wav_path}")
                return

            with wave.open(str(wav_path), "rb") as wf:
                channels = wf.getnchannels()
                sample_width = wf.getsampwidth()
                sample_rate = wf.getframerate()
                comptype = wf.getcomptype()

                print("[SERVER] WAV info:")
                print(f"  channels={channels}")
                print(f"  sample_width={sample_width}")
                print(f"  sample_rate={sample_rate}")
                print(f"  compression={comptype}")

                if channels != 1:
                    print("[SERVER] ERROR: WAV must be mono")
                    return
                if sample_width != 2:
                    print("[SERVER] ERROR: WAV must be 16-bit PCM")
                    return
                if sample_rate != 16000:
                    print("[SERVER] ERROR: WAV must be 16 kHz")
                    return
                if comptype != "NONE":
                    print("[SERVER] ERROR: WAV must be uncompressed PCM")
                    return

                warmup_packets = 4
                sent_packets = 0
                packet_duration = CHUNK_FRAMES / sample_rate

                while True:
                    frames = wf.readframes(CHUNK_FRAMES)
                    if not frames:
                        break

                    self.transport.sendto(frames, addr)
                    sent_packets += 1

                    if sent_packets > warmup_packets:
                        await asyncio.sleep(packet_duration)

                self.transport.sendto(b"__END__", addr)
                print(f"[SERVER] Stream finished for {addr}")

        except asyncio.CancelledError:
            print(f"[SERVER] Stream cancelled for {addr}")
            raise

        except Exception as exc:
            print(f"[SERVER] Stream error for {addr}: {exc}")

        finally:
            current_task = self.client_tasks.get(addr)
            if current_task is asyncio.current_task():
                self.client_tasks.pop(addr, None)

    def connection_lost(self, exc):
        print("[SERVER] UDP server closed")
        for task in self.client_tasks.values():
            task.cancel()
        self.client_tasks.clear()


async def main():
    loop = asyncio.get_running_loop()
    transport, protocol = await loop.create_datagram_endpoint(
        lambda: UDPAudioSender(),
        local_addr=(HOST, PORT),
    )

    print(f"[SERVER] Listening on udp://{HOST}:{PORT}")
    try:
        await asyncio.Future()
    finally:
        transport.close()


if __name__ == "__main__":
    asyncio.run(main())