import socket
import struct
import wave
import time
from datetime import datetime

LOG_FILE = "server_log.txt"

def log(message, level="INFO"):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    line = f"[{ts}] [{level}] {message}"
    print(line)
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(line + "\n")

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)  # 2MB
sock.bind(("", 5000))

log("Servidor UDP iniciado en puerto 5000", "START")

HEADER_SIZE = 8
wav = wave.open("output_jitter.wav", "wb")
wav.setnchannels(1)
wav.setsampwidth(2)
wav.setframerate(16000)

# Ajustes más tolerantes (porque usas paquetes de ~10ms)
JITTER_MS = 200                    # aumentamos el buffer inicial
JITTER_SIZE = 15                   # más paquetes iniciales
MAX_MISSING_BEFORE_LOSS = 12
MAX_BUFFER = 600

buffer = {}
expected_seq = None
last_packet_size = 0
total_received = 0
total_lost = 0
missing_counter = 0

log(f"Jitter buffer: {JITTER_SIZE} paquetes (~{JITTER_MS}ms)")

try:
    while True:
        data, addr = sock.recvfrom(4096)

        if data == b"PING":
            sock.sendto(b"PONG", addr)
            continue

        if len(data) < HEADER_SIZE:
            continue

        seq, ts = struct.unpack("<II", data[:HEADER_SIZE])
        audio = data[HEADER_SIZE:]

        buffer[seq] = audio
        last_packet_size = len(audio)
        total_received += 1

        if expected_seq is None:
            expected_seq = seq
            log(f"Primer paquete recibido → seq = {seq} | tamaño audio = {len(audio)} bytes", "INIT")
            continue

        # Procesar buffer
        while expected_seq in buffer:
            wav.writeframes(buffer[expected_seq])
            del buffer[expected_seq]
            expected_seq += 1
            missing_counter = 0

        # Pérdidas
        if expected_seq not in buffer:
            missing_counter += 1
            if missing_counter >= MAX_MISSING_BEFORE_LOSS:
                log(f"[LOSS] seq {expected_seq} (missing {missing_counter})", "LOSS")
                total_lost += 1
                if last_packet_size > 0:
                    wav.writeframes(b'\x00' * last_packet_size)
                expected_seq += 1
                missing_counter = 0

        # Limpieza
        if len(buffer) > MAX_BUFFER:
            log(f"[WARN] Buffer overflow ({len(buffer)}), limpiando...", "WARN")
            for old in list(buffer.keys()):
                if old < expected_seq - 50:
                    del buffer[old]

        if total_received % 300 == 0:
            loss_rate = total_lost / total_received * 100 if total_received else 0
            log(f"seq={expected_seq} | buf={len(buffer)} | rec={total_received} | lost={total_lost} ({loss_rate:.1f}%)", "STAT")

except KeyboardInterrupt:
    pass
finally:
    wav.close()
    sock.close()
    log(f"FINAL → Recibidos: {total_received} | Perdidos: {total_lost}", "SUMMARY")