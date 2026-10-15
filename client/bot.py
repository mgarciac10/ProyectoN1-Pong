"""Headless test client (bot) for PongServer. Version 2: register and join a match.

Usage examples:
    python bot.py --nick ana                          # register, print the reply, leave
    python bot.py --nick ana --hold 20                # stay connected 20 s, then send QUIT
    python bot.py --nick ana --join --hold 30         # register, JOIN, print what arrives for 30 s
    python bot.py --nick ana --email ana@x.co --host 1.2.3.4 --port 5000

To see ERROR(3) NICK_TAKEN, start a first bot with --hold and, meanwhile,
run a second bot with the same nick.
To see a match, run two bots with --join --hold 30 at the same time. Stop one
of them with Ctrl+C: the other one receives GAME_OVER (reason: opponent left).
"""
import argparse
import socket
import sys
import time

import protocol


def print_messages_for(sock, seconds):
    """Prints every message the server sends during `seconds` seconds."""
    deadline = time.time() + seconds
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            return True
        sock.settimeout(remaining)
        try:
            msg_type, payload = protocol.recv_msg(sock)
        except socket.timeout:
            return True
        except protocol.ConnectionClosed:
            print("The server closed the connection")
            return False
        print("RECV  %s" % protocol.describe(msg_type, payload))


def main():
    parser = argparse.ArgumentParser(description="PongServer test bot (v2: register + join)")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--nick", required=True)
    parser.add_argument("--email", default=None, help="default: <nick>@test.com")
    parser.add_argument("--join", action="store_true", help="send JOIN after REGISTER_OK")
    parser.add_argument("--hold", type=float, default=0,
                        help="seconds to stay connected (printing what arrives), then QUIT")
    args = parser.parse_args()

    email = args.email or (args.nick + "@test.com")

    # Same calls as in C: socket() -> connect() -> send()/recv() -> close()
    with socket.create_connection((args.host, args.port)) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("Connected to %s:%d" % (args.host, args.port))

        frame = protocol.send_msg(sock, protocol.MSG_REGISTER,
                                  protocol.encode_register(args.nick, email))
        print("SENT  REGISTER nick=%s email=%s  [%s]" % (args.nick, email, protocol.hex_bytes(frame)))

        try:
            msg_type, payload = protocol.recv_msg(sock)
        except protocol.ConnectionClosed:
            print("The server closed the connection")
            return 1
        print("RECV  %s" % protocol.describe(msg_type, payload))

        if msg_type != protocol.MSG_REGISTER_OK:
            return 1

        if args.join:
            frame = protocol.send_msg(sock, protocol.MSG_JOIN)
            print("SENT  JOIN  [%s]" % protocol.hex_bytes(frame))

        if args.hold > 0:
            print("Staying connected for %.0f s..." % args.hold)
            if not print_messages_for(sock, args.hold):
                return 1

        frame = protocol.send_msg(sock, protocol.MSG_QUIT)
        print("SENT  QUIT  [%s]" % protocol.hex_bytes(frame))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted: closing the connection without QUIT")
        sys.exit(130)
