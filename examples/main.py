import socket

SERVER_PORT = 5000
BUFFER_SIZE = 1024

clients = set()

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("", SERVER_PORT))

print(f"Servidor UDP escuchando en puerto {SERVER_PORT}")

while True:
    data, addr = sock.recvfrom(BUFFER_SIZE)
    message = data.decode()

    # registrar cliente
    clients.add(addr)

    print(f"Mensaje de {addr}: {message}")

    # Responder PING inmediatamente al que lo envió
    if message == "PING":
        sock.sendto(b"PONG", addr)
        continue  # opcional: no reenviar PING a otros clientes

    # reenviar a todos los demás clientes
    for client in clients:
        if client != addr:
            sock.sendto(data, client)