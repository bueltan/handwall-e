import socket
import struct
import time

SERVER_PORT = 5000
BUFFER_SIZE = 2048

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("", SERVER_PORT))

print(f"Servidor UDP escuchando en puerto {SERVER_PORT}")

HEADER_FORMAT = "<II"
HEADER_SIZE = 8

# Estado por cliente
last_sequence_per_client = {}

# Archivo de log
log_file = open("packets.log", "a")

while True:
    data, addr = sock.recvfrom(BUFFER_SIZE)

    # ================= PING =================
    if data == b"PING":
        sock.sendto(b"PONG", addr)
        continue

    # ================= VALIDACIÓN =================
    if len(data) < HEADER_SIZE:
        print(f"Paquete inválido de {addr}")
        continue

    # ================= PARSE =================
    header = data[:HEADER_SIZE]
    sequence, timestamp = struct.unpack(HEADER_FORMAT, header)

    # ================= DETECTAR PÉRDIDA =================
    last_seq = last_sequence_per_client.get(addr)

    lost = 0
    out_of_order = 0

    if last_seq is not None:
        if sequence > last_seq + 1:
            lost = sequence - (last_seq + 1)
        elif sequence <= last_seq:
            out_of_order = 1

    last_sequence_per_client[addr] = sequence

    # ================= LOG =================
    now = time.time()

    log_line = (
        f"time={now:.3f} "
        f"addr={addr[0]}:{addr[1]} "
        f"seq={sequence} "
        f"ts={timestamp} "
        f"lost={lost} "
        f"ooo={out_of_order}\n"
    )

    log_file.write(log_line)
    log_file.flush()

    print(log_line.strip())