"""Headless test client (bot) for PongServer. Version 3: register, join and play.

Usage examples:
    python bot.py --nick ana                          # register, print the reply, leave
    python bot.py --nick ana --hold 20                # stay connected 20 s, then send QUIT
    python bot.py --nick ana --join --hold 30         # register, JOIN, print what arrives for 30 s
    python bot.py --nick ana --play                   # register, JOIN and play a whole match
    python bot.py --nick ana --email ana@x.co --host 1.2.3.4 --port 5000

To see ERROR(3) NICK_TAKEN, start a first bot with --hold and, meanwhile,
run a second bot with the same nick.
To see a whole match, run two bots with --play at the same time. Each bot
moves its paddle toward the ball (with a small "reaction" error, so goals
happen). Stop one with Ctrl+C: the other receives GAME_OVER (opponent left).
"""
import argparse
import random
import socket
import sys
import time

import protocol


def print_messages_for(sock, seconds):
    """Prints every message the server sends during `seconds` seconds
    (STATE only when the score changes: it arrives 30 times per second)."""
    deadline = time.time() + seconds
    score = None
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
        if msg_type == protocol.MSG_STATE:
            new_score = protocol.decode_state(payload)[4:]
            if new_score != score:
                score = new_score
                print("RECV  STATE score %d-%d" % score)
            continue
        print("RECV  %s" % protocol.describe(msg_type, payload))


def play_match(sock):
    """Plays until GAME_OVER: follows the ball with the paddle.
    Sends INPUT only when the direction CHANGES (README rule 4)."""
    side = None
    current_dir = protocol.DIR_STOP
    score = None
    aim_error = 0
    prev_ball_x = None
    coming = False
    # While WAITING the server sends nothing (it may take minutes until an
    # opponent joins), so no timeout. Once playing, STATE arrives 30 times per
    # second: 10 s of silence means the server is gone.
    sock.settimeout(None)
    while True:
        msg_type, payload = protocol.recv_msg(sock)

        if msg_type == protocol.MSG_STATE and side is not None:
            ball_x, ball_y, left_y, right_y, score_l, score_r = protocol.decode_state(payload)
            if (score_l, score_r) != score:
                score = (score_l, score_r)
                print("RECV  STATE score %d-%d" % score)

            # Each time the ball starts coming toward us, pick a new aim error.
            # Half a paddle + half a ball is 45, so |error| > 45 means a miss (~25%).
            if prev_ball_x is not None and ball_x != prev_ball_x:
                now_coming = (ball_x < prev_ball_x) if side == 0 else (ball_x > prev_ball_x)
                if now_coming and not coming:
                    aim_error = random.randint(-60, 60)
                coming = now_coming
            prev_ball_x = ball_x

            paddle_y = left_y if side == 0 else right_y
            target = ball_y + protocol.BALL_SIZE / 2 + aim_error
            centre = paddle_y + protocol.PADDLE_H / 2
            if target < centre - 6:
                wanted = protocol.DIR_UP
            elif target > centre + 6:
                wanted = protocol.DIR_DOWN
            else:
                wanted = protocol.DIR_STOP

            if wanted != current_dir:
                current_dir = wanted
                protocol.send_msg(sock, protocol.MSG_INPUT, bytes([wanted]))
            continue

        print("RECV  %s" % protocol.describe(msg_type, payload))
        if msg_type == protocol.MSG_MATCH_START:
            side = protocol.decode_match_start(payload)[0]
            sock.settimeout(10)
        elif msg_type == protocol.MSG_GAME_OVER:
            return


def main():
    parser = argparse.ArgumentParser(description="PongServer test bot (v3: register, join, play)")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--nick", required=True)
    parser.add_argument("--email", default=None, help="default: <nick>@test.com")
    parser.add_argument("--join", action="store_true", help="send JOIN after REGISTER_OK")
    parser.add_argument("--play", action="store_true", help="JOIN and play one whole match")
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

        if args.join or args.play:
            frame = protocol.send_msg(sock, protocol.MSG_JOIN)
            print("SENT  JOIN  [%s]" % protocol.hex_bytes(frame))

        if args.play:
            try:
                play_match(sock)
            except protocol.ConnectionClosed:
                print("The server closed the connection")
                return 1
            except socket.timeout:
                print("No message from the server for 10 s during the match: giving up")
                return 1
        elif args.hold > 0:
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
