import asyncio
import struct
import wave
from pathlib import Path

HOST = "0.0.0.0"
PORT = 9999
WAV_FILE = "wall-e.wav"

# 20 ms @ 16 kHz
CHUNK_FRAMES = 320

# Packet format
MAGIC = b"ADP1"
VERSION = 1
CODEC_IMA_ADPCM = 1

# Header:
# 4s  magic             -> b"ADP1"
# B   version           -> 1
# B   codec             -> 1 (IMA ADPCM)
# H   reserved          -> 0
# I   seq               -> packet sequence
# I   timestamp_samples -> first PCM sample index of this packet
# H   samples           -> number of decoded PCM samples represented by this packet
# h   predictor         -> initial predictor for this packet
# B   step_index        -> initial step index for this packet
# B   channels          -> 1
# H   payload_size      -> ADPCM payload size in bytes
HEADER_STRUCT = struct.Struct("<4sBBHIIHhBBH")


IMA_STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552
, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]

IMA_INDEX_TABLE = [
    -1, -1, -1, -1,
     2,  4,  6,  8,
    -1, -1, -1, -1,
     2,  4,  6,  8,
]


def clamp(value: int, low: int, high: int) -> int:
    if value < low:
        return low
    if value > high:
        return high
    return value


class ImaAdpcmEncoder:
    def __init__(self, predictor: int = 0, step_index: int = 0):
        self.predictor = clamp(predictor, -32768, 32767)
        self.step_index = clamp(step_index, 0, 88)

    def encode_sample(self, sample: int) -> int:
        step = IMA_STEP_TABLE[self.step_index]
        diff = sample - self.predictor
        nibble = 0

        if diff < 0:
            nibble |= 8
            diff = -diff

        delta = step >> 3

        if diff >= step:
            nibble |= 4
            diff -= step
            delta += step

        if diff >= (step >> 1):
            nibble |= 2
            diff -= (step >> 1)
            delta += (step >> 1)

        if diff >= (step >> 2):
            nibble |= 1
            delta += (step >> 2)

        if nibble & 8:
            self.predictor -= delta
        else:
            self.predictor += delta

        self.predictor = clamp(self.predictor, -32768, 32767)
        self.step_index = clamp(self.step_index + IMA_INDEX_TABLE[nibble & 0x0F], 0, 88)

        return nibble & 0x0F

    def encode_block(self, pcm16_bytes: bytes):
        if len(pcm16_bytes) % 2 != 0:
            raise ValueError("PCM16 byte length must be even")

        samples = struct.unpack("<{}h".format(len(pcm16_bytes) // 2), pcm16_bytes)

        initial_predictor = self.predictor
        initial_step_index = self.step_index

        nibbles = []
        for s in samples:
            nibbles.append(self.encode_sample(s))

        payload = bytearray()
        for i in range(0, len(nibbles), 2):
            lo = nibbles[i]
            hi = nibbles[i + 1] if (i + 1) < len(nibbles) else 0
            payload.append((hi << 4) | lo)

        return {
            "predictor": initial_predictor,
            "step_index": initial_step_index,
            "samples": len(samples),
            "payload": bytes(payload),
        }


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

                encoder = ImaAdpcmEncoder()
                seq = 0
                timestamp_samples = 0
                sent_packets = 0
                warmup_packets = 12
                packet_duration = CHUNK_FRAMES / sample_rate

                print(
                    f"[SERVER] Streaming IMA ADPCM to {addr} | "
                    f"chunk_frames={CHUNK_FRAMES} | packet_duration={packet_duration*1000:.1f} ms"
                )

                while True:
                    pcm16 = wf.readframes(CHUNK_FRAMES)
                    if not pcm16:
                        break

                    block = encoder.encode_block(pcm16)
                    payload = block["payload"]
                    samples_in_packet = block["samples"]
                    predictor = block["predictor"]
                    step_index = block["step_index"]

                    header = HEADER_STRUCT.pack(
                        MAGIC,
                        VERSION,
                        CODEC_IMA_ADPCM,
                        0,  # reserved
                        seq,
                        timestamp_samples,
                        samples_in_packet,
                        predictor,
                        step_index,
                        1,  # channels
                        len(payload),
                    )

                    packet = header + payload
                    self.transport.sendto(packet, addr)

                    sent_packets += 1

                    print(
                        "[SERVER] Sent packet | "
                        f"addr={addr} | seq={seq} | ts_samples={timestamp_samples} | "
                        f"samples={samples_in_packet} | predictor={predictor} | "
                        f"step_index={step_index} | payload={len(payload)} bytes | total={len(packet)} bytes"
                    )

                    seq += 1
                    timestamp_samples += samples_in_packet

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