"""PongProtocol v1 on the client side (same definitions as server/protocol.h).

Wire format of every message (see README section 2.3):

    +---------+------+-----------------+----------------------+
    | version | type | length (u16 BE) | payload (length B)   |
    |   1 B   | 1 B  |      2 B        |     0 - 255 B        |
    +---------+------+-----------------+----------------------+

Only the plain `socket` and `struct` modules are used, like the C server uses
the raw Berkeley API.
"""
import socket
import struct

PROTO_VERSION = 1
HEADER_SIZE = 4
MAX_PAYLOAD = 255

# "!BBH" = network byte order (big-endian): u8 version, u8 type, u16 length.
HEADER_FORMAT = "!BBH"

# Message types (the high bit 0x80 marks messages sent by the server).
MSG_REGISTER = 0x01
MSG_JOIN = 0x02
MSG_INPUT = 0x03
MSG_QUIT = 0x04
MSG_REGISTER_OK = 0x81
MSG_WAITING = 0x82
MSG_MATCH_START = 0x83
MSG_STATE = 0x84
MSG_GAME_OVER = 0x85
MSG_ERROR = 0xFF

# Error codes (payload of MSG_ERROR).
ERR_BAD_VERSION = 1
ERR_BAD_MESSAGE = 2
ERR_NICK_TAKEN = 3
ERR_INVALID_STATE = 4
ERR_INVALID_FIELD = 5

TYPE_NAMES = {
    MSG_REGISTER: "REGISTER", MSG_JOIN: "JOIN", MSG_INPUT: "INPUT", MSG_QUIT: "QUIT",
    MSG_REGISTER_OK: "REGISTER_OK", MSG_WAITING: "WAITING", MSG_MATCH_START: "MATCH_START",
    MSG_STATE: "STATE", MSG_GAME_OVER: "GAME_OVER", MSG_ERROR: "ERROR",
}
ERROR_NAMES = {
    ERR_BAD_VERSION: "BAD_VERSION", ERR_BAD_MESSAGE: "BAD_MESSAGE", ERR_NICK_TAKEN: "NICK_TAKEN",
    ERR_INVALID_STATE: "INVALID_STATE", ERR_INVALID_FIELD: "INVALID_FIELD",
}


class ConnectionClosed(Exception):
    """The server closed the connection."""


class ProtocolError(Exception):
    """The server sent something that breaks the protocol."""


def encode(msg_type, payload=b""):
    """Builds the bytes of one message: header + payload."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long (max %d bytes)" % MAX_PAYLOAD)
    return struct.pack(HEADER_FORMAT, PROTO_VERSION, msg_type, len(payload)) + payload


def encode_register(nick, email):
    """REGISTER payload: nick_len:u8, nick, email_len:u8, email (ASCII, no terminating NUL)."""
    nick_b = nick.encode("ascii")
    email_b = email.encode("ascii")
    return bytes([len(nick_b)]) + nick_b + bytes([len(email_b)]) + email_b


def send_msg(sock, msg_type, payload=b""):
    """Sends one message and returns the exact bytes that went on the wire."""
    frame = encode(msg_type, payload)
    sock.sendall(frame)          # sendall() = the Python version of our C send_all()
    return frame


def recv_exact(sock, n):
    """Reads EXACTLY n bytes. TCP is a byte stream, so one recv() may return fewer."""
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:            # b"" means the peer closed the connection
            raise ConnectionClosed()
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_msg(sock):
    """Reads one whole message. Returns (msg_type, payload)."""
    header = recv_exact(sock, HEADER_SIZE)
    version, msg_type, length = struct.unpack(HEADER_FORMAT, header)
    if version != PROTO_VERSION:
        raise ProtocolError("unexpected protocol version %d" % version)
    payload = recv_exact(sock, length) if length else b""
    return msg_type, payload


def hex_bytes(data):
    """b'\\x01\\x81' -> '01 81'."""
    return data.hex(" ")


def describe(msg_type, payload):
    """Human-readable text for a message received from the server."""
    name = TYPE_NAMES.get(msg_type, "UNKNOWN(0x%02x)" % msg_type)
    if msg_type == MSG_ERROR and len(payload) == 1:
        return "ERROR code=%d (%s)" % (payload[0], ERROR_NAMES.get(payload[0], "?"))
    if payload:
        return "%s payload=[%s]" % (name, hex_bytes(payload))
    return name