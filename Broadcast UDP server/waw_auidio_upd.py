import socket
import struct
import wave

SERVER_PORT = 5000
BUFFER_SIZE = 2048

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("", SERVER_PORT))

print("Listening...")

HEADER_FORMAT = "<II"
HEADER_SIZE = 8

# Create WAV file
wav = wave.open("output.wav", "wb")
wav.setnchannels(1)        # mono
wav.setsampwidth(2)        # 16-bit
wav.setframerate(16000)    # 16 kHz

while True:
    data, addr = sock.recvfrom(BUFFER_SIZE)

    if data == b"PING":
        sock.sendto(b"PONG", addr)
        continue

    if len(data) < HEADER_SIZE:
        continue

    # Split header + audio
    header = data[:HEADER_SIZE]
    audio = data[HEADER_SIZE:]

    sequence, timestamp = struct.unpack(HEADER_FORMAT, header)

    # 🔥 Write raw audio directly (NO FIXES)
    wav.writeframes(audio)

    print(f"seq={sequence} bytes={len(audio)}")