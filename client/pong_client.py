"""PongClient: pygame client for PongServer (PongProtocol v1, README section 2.3).

Usage:
    python pong_client.py [--host HOST] [--port PORT] [--nick NICK] [--email EMAIL]

Registration happens in the console (it asks for the nickname and e-mail if
they are not given), then the game window opens. The menu shows the players
that are available right now (refreshed every 1.5 s):
    1-9 = play against that player     J = become available (others can choose you)
    Up / Down (or W / S) = move your paddle      Q / Esc = quit

Design:
  * A RECEIVER THREAD blocks on recv_msg() and puts every message in a queue.
    recv() blocks, so doing it in the pygame loop would freeze the window.
  * The MAIN THREAD runs the pygame loop: it empties the queue, updates what
    it shows, draws, and sends INPUT only when the paddle direction changes.
    Only the main thread sends, so no lock is needed for the socket.
  * The client only draws what the server says (STATE): the server owns the game.
"""
import argparse
import queue
import socket
import sys
import threading
import time

import pygame

import protocol

FPS = 60
LIST_EVERY = 1.5            # seconds between LIST requests while in the menu
BG = (15, 15, 25)
FG = (230, 230, 230)
MINE = (90, 200, 255)       # colour of the player's own paddle
DIM = (120, 120, 140)


# ------------------------------------------------------------------ network

def connect_and_register(host, port, nick, email):
    """Connects and registers in the console. On NICK_TAKEN / INVALID_FIELD it
    asks again and retries on the same connection (README rule 1)."""
    sock = socket.create_connection((host, port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print("Connected to %s:%d" % (host, port))

    while True:
        nick = nick or input("Nickname (1-16 letters, digits, _ or -): ").strip()
        email = email or input("E-mail: ").strip()
        try:
            payload = protocol.encode_register(nick, email)
        except (UnicodeEncodeError, ValueError):
            print("Use only ASCII characters (and at most 255 bytes).")
            nick = email = None
            continue

        protocol.send_msg(sock, protocol.MSG_REGISTER, payload)
        msg_type, reply = protocol.recv_msg(sock)

        if msg_type == protocol.MSG_REGISTER_OK:
            print("Registered as '%s'." % nick)
            return sock, nick
        if msg_type == protocol.MSG_ERROR and reply and reply[0] == protocol.ERR_NICK_TAKEN:
            print("That nickname is already in use, choose another one.")
            nick = None
        elif msg_type == protocol.MSG_ERROR and reply and reply[0] == protocol.ERR_INVALID_FIELD:
            print("Invalid nickname or e-mail, try again.")
            nick = email = None
        else:
            raise protocol.ProtocolError("unexpected reply: " + protocol.describe(msg_type, reply))


def receiver(sock, inbox):
    """Receiver thread: puts (msg_type, payload) in the queue; None when the connection ends."""
    try:
        while True:
            inbox.put(protocol.recv_msg(sock))
    except (protocol.ConnectionClosed, protocol.ProtocolError, OSError):
        inbox.put(None)


# --------------------------------------------------------------------- game

class PongClient:
    def __init__(self, sock, nick):
        self.sock = sock
        self.nick = nick
        self.inbox = queue.Queue()
        self.screen = None   # "menu", "waiting", "playing", "disconnected"
        self.side = 0
        self.opponent = ""
        self.state = None    # last STATE: (ball_x, ball_y, left_y, right_y, score_l, score_r)
        self.result = ""     # result of the last match, shown in the menu
        self.available = []  # nicks from the last USER_LIST
        self.status = ""     # short message (e.g. "bob is no longer available")
        self.status_until = 0.0
        self.last_list = 0.0
        self.current_dir = protocol.DIR_STOP
        self.held = set()    # keys held down (tracked from KEYDOWN / KEYUP events)

    def send(self, msg_type, payload=b""):
        try:
            protocol.send_msg(self.sock, msg_type, payload)
        except OSError:
            self.screen = "disconnected"

    # ----- messages from the server

    def handle(self, msg):
        if msg is None:
            self.screen = "disconnected"
            return
        msg_type, payload = msg
        if msg_type == protocol.MSG_WAITING:
            self.screen = "waiting"
        elif msg_type == protocol.MSG_USER_LIST:
            self.available = protocol.decode_user_list(payload)
        elif msg_type == protocol.MSG_MATCH_START:
            self.side, self.opponent = protocol.decode_match_start(payload)
            self.state = None
            self.current_dir = protocol.DIR_STOP
            self.screen = "playing"
        elif msg_type == protocol.MSG_STATE:
            self.state = protocol.decode_state(payload)
        elif msg_type == protocol.MSG_GAME_OVER:
            winner, reason = protocol.decode_game_over(payload)
            won = (winner == self.side)
            if reason == 1:
                self.result = "%s left the match. You win!" % self.opponent
            else:
                self.result = "You win!" if won else "%s wins." % self.opponent
                if self.state:   # the last STATE carries the final score
                    self.result += "  (%d-%d)" % (self.state[4], self.state[5])
            self.screen = "menu"    # back to REGISTERED on the server: choose again
            self.last_list = 0.0    # refresh the list right away
        elif msg_type == protocol.MSG_ERROR:
            if payload and payload[0] == protocol.ERR_USER_NOT_AVAILABLE:
                self.show_status("That player is no longer available")
            print("Server: " + protocol.describe(msg_type, payload))

    def show_status(self, text, seconds=3.0):
        self.status, self.status_until = text, time.time() + seconds

    # ----- keyboard

    def wanted_direction(self):
        up = pygame.K_UP in self.held or pygame.K_w in self.held
        down = pygame.K_DOWN in self.held or pygame.K_s in self.held
        if up and not down:
            return protocol.DIR_UP
        if down and not up:
            return protocol.DIR_DOWN
        return protocol.DIR_STOP

    def on_key(self, key):
        if key in (pygame.K_q, pygame.K_ESCAPE):
            return False
        if key == pygame.K_j and self.screen == "menu":
            self.send(protocol.MSG_JOIN)          # the server answers WAITING
        elif pygame.K_1 <= key <= pygame.K_9 and self.screen in ("menu", "waiting"):
            i = key - pygame.K_1
            if i < len(self.available):
                nick = self.available[i]
                self.send(protocol.MSG_PLAY_WITH, protocol.encode_nick(nick))
                self.show_status("Asking to play against %s..." % nick)
        return True

    # ----- drawing

    def text(self, surface, font, msg, y, colour=FG):
        img = font.render(msg, True, colour)
        surface.blit(img, ((protocol.FIELD_W - img.get_width()) // 2, y))

    def draw(self, surface, big, small):
        surface.fill(BG)
        if self.screen == "playing" and self.state:
            bx, by, ly, ry, sl, sr = self.state
            for y in range(0, protocol.FIELD_H, 30):       # centre line
                pygame.draw.rect(surface, DIM, (protocol.FIELD_W // 2 - 1, y, 2, 15))
            for side, (x, y) in enumerate([(protocol.PADDLE_X_LEFT, ly), (protocol.PADDLE_X_RIGHT, ry)]):
                colour = MINE if side == self.side else FG
                pygame.draw.rect(surface, colour, (x, y, protocol.PADDLE_W, protocol.PADDLE_H))
            pygame.draw.rect(surface, FG, (bx, by, protocol.BALL_SIZE, protocol.BALL_SIZE))
            names = (self.nick, self.opponent) if self.side == 0 else (self.opponent, self.nick)
            self.text(surface, big, "%d   %d" % (sl, sr), 20)
            self.text(surface, small, "%s  vs  %s" % names, 70, DIM)
        elif self.screen == "playing":
            self.text(surface, big, "Match vs %s" % self.opponent, 250)
        elif self.screen == "disconnected":
            self.text(surface, big, "Connection to the server lost", 250)
            self.text(surface, small, "Q = close", 320, DIM)
        else:  # menu or waiting: the list of available players
            self.text(surface, big, self.result or "PONG", 60)
            if self.screen == "waiting":
                self.text(surface, small, "You are available: waiting for someone to choose you...", 130, MINE)
            else:
                self.text(surface, small, "Registered as '%s'" % self.nick, 130, DIM)
            self.text(surface, small, "Available players:", 190)
            if not self.available:
                self.text(surface, small, "(nobody right now)", 230, DIM)
            for i, nick in enumerate(self.available[:9]):
                self.text(surface, small, "%d.  %s" % (i + 1, nick), 230 + 34 * i)
            keys = "1-9 = play against     Q = quit" if self.screen == "waiting" else                    "1-9 = play against     J = be available     Q = quit"
            self.text(surface, small, keys, 550, DIM)
        if self.status and time.time() < self.status_until:
            self.text(surface, small, self.status, 510, MINE)
        pygame.display.flip()

    # ----- main loop

    def run(self):
        threading.Thread(target=receiver, args=(self.sock, self.inbox), daemon=True).start()
        pygame.init()
        surface = pygame.display.set_mode((protocol.FIELD_W, protocol.FIELD_H))
        pygame.display.set_caption("Pong - %s" % self.nick)
        big, small = pygame.font.SysFont(None, 56), pygame.font.SysFont(None, 30)
        clock = pygame.time.Clock()
        self.screen = "menu"

        running = True
        while running:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    running = False
                elif event.type == pygame.KEYDOWN:
                    self.held.add(event.key)
                    running = self.on_key(event.key)
                elif event.type == pygame.KEYUP:
                    self.held.discard(event.key)
                elif event.type == pygame.WINDOWFOCUSLOST:
                    self.held.clear()   # a key released in another window never sends KEYUP here

            while not self.inbox.empty():
                self.handle(self.inbox.get())

            # The list of available players changes all the time: ask again regularly.
            if self.screen in ("menu", "waiting") and time.time() - self.last_list > LIST_EVERY:
                self.send(protocol.MSG_LIST)
                self.last_list = time.time()

            # README rule 4: INPUT only when the direction changes, not every frame.
            if self.screen == "playing":
                wanted = self.wanted_direction()
                if wanted != self.current_dir:
                    self.current_dir = wanted
                    self.send(protocol.MSG_INPUT, bytes([wanted]))

            self.draw(surface, big, small)
            clock.tick(FPS)

        if self.screen != "disconnected":
            self.send(protocol.MSG_QUIT)
        self.sock.close()
        pygame.quit()


def main():
    parser = argparse.ArgumentParser(description="PongClient (pygame)")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--nick")
    parser.add_argument("--email")
    args = parser.parse_args()

    try:
        sock, nick = connect_and_register(args.host, args.port, args.nick, args.email)
    except OSError as e:
        print("Cannot connect to %s:%d: %s" % (args.host, args.port, e))
        return 1
    except (protocol.ConnectionClosed, protocol.ProtocolError) as e:
        print("Registration failed: %s" % (e or "the server closed the connection"))
        return 1

    PongClient(sock, nick).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
