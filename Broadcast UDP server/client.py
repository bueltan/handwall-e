import socket
import threading

SERVER_IP = "192.168.1.37"   # IP del servidor
SERVER_PORT = 5000

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def receive():
    while True:
        data, _ = sock.recvfrom(1024)
        print("Recibido:", data.decode())

threading.Thread(target=receive, daemon=True).start()

while True:
    msg = input("Mensaje: ")
    sock.sendto(msg.encode(), (SERVER_IP, SERVER_PORT))