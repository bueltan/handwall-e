import socket
import struct
import wave
from datetime import datetime

# ====================== CONFIGURACIÓN ======================
SERVER_PORT = 5000
BUFFER_SIZE = 4096

SAMPLE_RATE = 16000
SAMPLES_PER_PACKET = 160
PACKET_AUDIO_SIZE = SAMPLES_PER_PACKET * 2  # 320 bytes
METADATA_SIZE = 4
PACKET_DURATION_MS = (SAMPLES_PER_PACKET / SAMPLE_RATE) * 1000  # 10.0 ms
JITTER_MS = 250                    # Buffer de jitter (recomendado)
JITTER_SIZE = max(10, int(JITTER_MS / PACKET_DURATION_MS))
MAX_BUFFER = 800
MAX_MISSING_BEFORE_LOSS = 5

# ====================== ARCHIVOS CON FECHA/HORA ======================
now = datetime.now()
timestamp = now.strftime("%Y-%m-%d_%H-%M-%S")

LOG_FILE = f"server_log_{timestamp}.txt"
WAV_FILE = f"audio_{timestamp}.wav"

# ====================== SETUP ======================
def log(message, level="INFO"):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    line = f"[{ts}] [{level}] {message}"
    print(line)
    
    # Guardar en archivo
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception as e:
        print(f"Error escribiendo log: {e}")

log("Servidor UDP iniciado", "START")
log(f"Archivo de audio: {WAV_FILE}")
log(f"Archivo de logs: {LOG_FILE}")
log(f"Jitter buffer: {JITTER_SIZE} paquetes (~{JITTER_SIZE * PACKET_DURATION_MS:.0f} ms)")

# Socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)  # 4MB
sock.bind(("", SERVER_PORT))

# WAV con nombre dinámico
wav = wave.open(WAV_FILE, "wb")
wav.setnchannels(1)
wav.setsampwidth(2)
wav.setframerate(SAMPLE_RATE)

# Variables
buffer = {}
expected_seq = None
last_packet_size = PACKET_AUDIO_SIZE
total_received = 0
total_lost = 0
missing_counter = 0

try:
    while True:
        data, addr = sock.recvfrom(BUFFER_SIZE)

        if data == b"PING":
            sock.sendto(b"PONG", addr)
            continue

        if len(data) < METADATA_SIZE:
            continue

        # Parse header
        seq = struct.unpack("<I", data[:METADATA_SIZE])[0]
        audio = data[METADATA_SIZE:]

        if len(audio) != PACKET_AUDIO_SIZE:
            log(f"Paquete con tamaño inválido: {len(audio)} bytes (esperado {PACKET_AUDIO_SIZE})", "WARN")
            continue

        buffer[seq] = audio
        last_packet_size = len(audio)
        total_received += 1

        if expected_seq is None:
            expected_seq = seq
            log(f"Primer paquete recibido → seq={seq} | tamaño={len(audio)} bytes", "INIT")
            continue

        # Procesar buffer en orden
        processed = 0
        while expected_seq in buffer and processed < 10:
            wav.writeframes(buffer[expected_seq])
            del buffer[expected_seq]
            expected_seq += 1
            missing_counter = 0
            processed += 1

        if expected_seq not in buffer and len(buffer) > 0:
            missing_counter += 1
            if missing_counter >= MAX_MISSING_BEFORE_LOSS:
                log(f"[LOSS] seq {expected_seq} perdido (faltan {missing_counter} consecutivos)", "LOSS")
                # Rellenar con silencio
                wav.writeframes(b'\x00' * last_packet_size)
                expected_seq += 1
                missing_counter = 0
                total_lost += 1

        # Limpieza de buffer viejo
        if len(buffer) > MAX_BUFFER:
            old_count = len(buffer)
            for old in list(buffer.keys()):
                if old < expected_seq - 10:
                    del buffer[old]

        # Estadísticas cada 300 paquetes
        if total_received % 300 == 0:
            loss_rate = (total_lost / total_received * 100) if total_received > 0 else 0
            log(f"[STAT] seq={expected_seq} | buf={len(buffer)} | rec={total_received} | "
                f"lost={total_lost} ({loss_rate:.2f}%)", "STAT")

except KeyboardInterrupt:
    log("Servidor detenido por el usuario", "STOP")
except Exception as e:
    log(f"Error inesperado: {e}", "ERROR")
finally:
    wav.close()
    sock.close()
    
    final_loss_rate = (total_lost / total_received * 100) if total_received > 0 else 0
    log(f"=== FIN DE TRANSMISIÓN ===", "SUMMARY")
    log(f"Archivo de audio guardado: {WAV_FILE}", "SUMMARY")
    log(f"Total paquetes recibidos: {total_received}", "SUMMARY")
    log(f"Total paquetes perdidos: {total_lost} ({final_loss_rate:.2f}%)", "SUMMARY")