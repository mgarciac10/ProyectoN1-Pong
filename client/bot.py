"""Headless test client (bot) for PongServer. Version 4: register, choose an opponent, play.

Usage examples:
    python bot.py --nick ana                          # register, print the reply, leave
    python bot.py --nick ana --hold 20                # stay connected 20 s, then send QUIT
    python bot.py --nick ana --list                   # print the list of available players
    python bot.py --nick ana --join --hold 30         # become available, print what arrives for 30 s
    python bot.py --nick ana --challenge bob --play   # choose 'bob' (when available) and play
    python bot.py --nick ana --play                   # find any opponent and play a whole match
    python bot.py --nick ana --email ana@x.co --host 1.2.3.4 --port 5000

With --play (and no --challenge) the bot asks for the list every second: it
chooses the first available player, or, if nobody is available, becomes
available itself (JOIN) and keeps looking until somebody is found.
During the match it moves its paddle toward the ball (with a small aiming
error, so goals happen). Stop one with Ctrl+C: the other receives GAME_OVER.
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


def send(sock, msg_type, payload=b"", text=None):
    frame = protocol.send_msg(sock, msg_type, payload)
    print("SENT  %s  [%s]" % (text or protocol.TYPE_NAMES[msg_type], protocol.hex_bytes(frame)))


def find_match(sock, target):
    """Gets into a match and returns our side (0 = left, 1 = right).
    target = a nick to challenge, or None to take any available player."""
    joined = False
    last_list = None
    while True:
        if target:
            send(sock, protocol.MSG_PLAY_WITH, protocol.encode_nick(target), "PLAY_WITH %s" % target)
        else:
            protocol.send_msg(sock, protocol.MSG_LIST)

        # Read replies for one second, then ask again.
        deadline = time.time() + 1.0
        while time.time() < deadline:
            sock.settimeout(max(0.05, deadline - time.time()))
            try:
                msg_type, payload = protocol.recv_msg(sock)
            except socket.timeout:
                break

            if msg_type == protocol.MSG_USER_LIST:
                nicks = protocol.decode_user_list(payload)
                if nicks != last_list:
                    print("RECV  USER_LIST %s" % nicks)
                    last_list = nicks
                if nicks:
                    send(sock, protocol.MSG_PLAY_WITH, protocol.encode_nick(nicks[0]), "PLAY_WITH %s" % nicks[0])
                elif not joined:
                    send(sock, protocol.MSG_JOIN)      # nobody available: offer ourselves
                    joined = True
                continue

            print("RECV  %s" % protocol.describe(msg_type, payload))
            if msg_type == protocol.MSG_MATCH_START:
                return protocol.decode_match_start(payload)[0]
            # ERROR(6): the chosen player was taken by someone else. ERROR(4): two
            # players chose each other at once. In both cases: just try again.


def play_match(sock, side):
    """Plays until GAME_OVER: follows the ball with the paddle.
    Sends INPUT only when the direction CHANGES (README rule 4)."""
    current_dir = protocol.DIR_STOP
    score = None
    aim_error = 0
    prev_ball_x = None
    coming = False
    sock.settimeout(10)   # STATE arrives 30 times per second: 10 s of silence = server gone
    while True:
        msg_type, payload = protocol.recv_msg(sock)

        if msg_type == protocol.MSG_STATE:
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
            # Dead zone of ±12: wider than one paddle step (9 per tick), otherwise
            # the paddle overshoots every tick and the bot floods the server with INPUT.
            if target < centre - 12:
                wanted = protocol.DIR_UP
            elif target > centre + 12:
                wanted = protocol.DIR_DOWN
            else:
                wanted = protocol.DIR_STOP

            if wanted != current_dir:
                current_dir = wanted
                protocol.send_msg(sock, protocol.MSG_INPUT, bytes([wanted]))
            continue

        print("RECV  %s" % protocol.describe(msg_type, payload))
        if msg_type == protocol.MSG_GAME_OVER:
            return


def main():
    parser = argparse.ArgumentParser(description="PongServer test bot (v4: register, choose, play)")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--nick", required=True)
    parser.add_argument("--email", default=None, help="default: <nick>@test.com")
    parser.add_argument("--list", action="store_true", help="print the list of available players")
    parser.add_argument("--join", action="store_true", help="become available (JOIN) after REGISTER_OK")
    parser.add_argument("--challenge", metavar="NICK", help="choose this player as opponent (PLAY_WITH)")
    parser.add_argument("--play", action="store_true", help="find an opponent and play one whole match")
    parser.add_argument("--hold", type=float, default=0,
                        help="seconds to stay connected (printing what arrives), then QUIT")
    args = parser.parse_args()

    email = args.email or (args.nick + "@test.com")

    # Same calls as in C: socket() -> connect() -> send()/recv() -> close()
    with socket.create_connection((args.host, args.port)) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("Connected to %s:%d" % (args.host, args.port))

        send(sock, protocol.MSG_REGISTER, protocol.encode_register(args.nick, email),
             "REGISTER nick=%s email=%s" % (args.nick, email))
        try:
            msg_type, payload = protocol.recv_msg(sock)
        except protocol.ConnectionClosed:
            print("The server closed the connection")
            return 1
        print("RECV  %s" % protocol.describe(msg_type, payload))
        if msg_type != protocol.MSG_REGISTER_OK:
            return 1

        try:
            if args.list:
                send(sock, protocol.MSG_LIST)
                print("RECV  %s" % protocol.describe(*protocol.recv_msg(sock)))
            if args.play:
                side = find_match(sock, args.challenge)
                play_match(sock, side)
            else:
                if args.join:
                    send(sock, protocol.MSG_JOIN)
                if args.challenge:
                    send(sock, protocol.MSG_PLAY_WITH, protocol.encode_nick(args.challenge),
                         "PLAY_WITH %s" % args.challenge)
                if args.hold > 0:
                    print("Staying connected for %.0f s..." % args.hold)
                    if not print_messages_for(sock, args.hold):
                        return 1
        except protocol.ConnectionClosed:
            print("The server closed the connection")
            return 1
        except socket.timeout:
            print("No message from the server for 10 s during the match: giving up")
            return 1

        send(sock, protocol.MSG_QUIT)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted: closing the connection without QUIT")
        sys.exit(130)
