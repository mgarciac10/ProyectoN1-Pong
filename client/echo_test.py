"""Minimal TCP echo client to test the Phase 1 server. Will be removed later.

Usage: python echo_test.py [host] [port]   (defaults: localhost 5000)
Type a line to send it; an empty line quits.
"""
import socket
import sys

host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

# Same Berkeley calls as in C: socket() -> connect() -> send()/recv() -> close()
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.connect((host, port))
    print(f"Connected to {host}:{port}. Type text (empty line to quit).")

    while True:
        line = input("> ")
        if not line:
            break
        sock.sendall(line.encode() + b"\n")

        # Good enough for a test: one recv() usually returns the whole short echo.
        data = sock.recv(1024)
        if not data:
            print("Server closed the connection")
            break
        print("echo:", data.decode(errors="replace").rstrip("\n"))
