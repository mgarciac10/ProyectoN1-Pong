/*
 * PongProtocol v1: framing, encoding and validation (see README section 2.3).
 *
 * This module knows NOTHING about sessions, threads or the logger: it only
 * turns bytes into messages and messages into bytes. Keeping it separate means
 * the protocol can be read (and tested) on its own.
 *
 * Wire format of every message:
 *     +---------+------+-----------------+----------------------+
 *     | version | type | length (u16 BE) | payload (length B)   |
 *     |   1 B   | 1 B  |      2 B        |     0 - 255 B        |
 *     +---------+------+-----------------+----------------------+
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_VERSION      1
#define PROTO_HEADER_SIZE  4
#define PROTO_MAX_PAYLOAD  255

/* Field limits of REGISTER (README 2.3.4). */
#define NICK_MIN   1
#define NICK_MAX   16
#define EMAIL_MIN  3
#define EMAIL_MAX  64

/* Message types. The high bit (0x80) marks messages sent by the server. */
typedef enum {
    MSG_REGISTER    = 0x01,
    MSG_JOIN        = 0x02,
    MSG_INPUT       = 0x03,
    MSG_QUIT        = 0x04,
    MSG_LIST        = 0x05,
    MSG_PLAY_WITH   = 0x06,
    MSG_REGISTER_OK = 0x81,
    MSG_WAITING     = 0x82,
    MSG_MATCH_START = 0x83,
    MSG_STATE       = 0x84,
    MSG_GAME_OVER   = 0x85,
    MSG_USER_LIST   = 0x86,
    MSG_ERROR       = 0xFF
} MsgType;

/* Payload of an ERROR message (README 2.3.5). */
typedef enum {
    ERR_BAD_VERSION   = 1,
    ERR_BAD_MESSAGE   = 2,
    ERR_NICK_TAKEN    = 3,
    ERR_INVALID_STATE = 4,
    ERR_INVALID_FIELD = 5,
    ERR_USER_NOT_AVAILABLE = 6
} ErrorCode;

/* The 4-byte header, already converted to host byte order. */
typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint16_t length;
} ProtoHeader;

typedef enum {
    PROTO_OK = 0,
    PROTO_CLOSED,       /* the peer closed the connection */
    PROTO_IO_ERROR,     /* send()/recv() failed: look at errno */
    PROTO_BAD_VERSION,  /* header.version != 1                -> ERROR(1) */
    PROTO_BAD_LENGTH,   /* header.length > 255                -> ERROR(2) */
    PROTO_BAD_TYPE      /* not a message a client may send    -> ERROR(2) */
} ProtoStatus;

/* ---- Framing: TCP is a byte stream, so we loop until we have ALL the bytes ---- */

/* Sends exactly len bytes (send() may send fewer). PROTO_OK or PROTO_IO_ERROR. */
ProtoStatus send_all(int fd, const void *buf, size_t len);

/* Receives exactly len bytes (recv() may return fewer).
 * PROTO_OK, PROTO_CLOSED (peer closed before we got them all) or PROTO_IO_ERROR. */
ProtoStatus recv_all(int fd, void *buf, size_t len);

/* ---- Whole messages ---- */

/* Builds header + payload in ONE buffer and sends it with a single send_all(). */
ProtoStatus proto_send(int fd, MsgType type, const uint8_t *payload, size_t len);

/*
 * Server-side receive: reads one message sent by a client.
 * Validation order (README 2.3.5): version -> length -> type. The payload is
 * read only if the three checks pass, because after a bad header we can no
 * longer trust the stream (the caller must answer with ERROR and close).
 * `payload` must have room for `max` bytes (use PROTO_MAX_PAYLOAD).
 */
ProtoStatus proto_recv(int fd, ProtoHeader *hdr, uint8_t *payload, size_t max);

/* ---- Decoding of payloads ---- */

typedef enum {
    DECODE_OK,
    DECODE_BAD_LAYOUT,  /* the bytes do not fit the message layout -> ERROR(2) */
    DECODE_BAD_FIELD    /* layout is fine but a field breaks its rule -> ERROR(5) */
} DecodeStatus;

/*
 * Decodes a REGISTER payload. `nick` needs NICK_MAX + 1 bytes and `email`
 * EMAIL_MAX + 1 bytes: both come back as normal NUL-terminated C strings.
 * On failure, *why points to a short static explanation (safe to log).
 */
DecodeStatus proto_decode_register(const uint8_t *payload, size_t len,
                                   char *nick, char *email, const char **why);

/* ---- Helpers for logging ---- */

/*
 * Decodes a PLAY_WITH payload: nick_len:u8, nick. The header's length must be
 * exactly 1 + nick_len. `nick` needs NICK_MAX + 1 bytes (comes back NUL-terminated).
 */
DecodeStatus proto_decode_nick(const uint8_t *payload, size_t len, char *nick, const char **why);

int         proto_is_client_type(uint8_t type);
const char *proto_type_name(uint8_t type);
const char *proto_error_name(uint8_t code);

/* "01 01 00 0d 03 ..." of the whole frame (header + payload), cut with "..."
 * if it does not fit in out. Hex is always safe to write into the log. */
void proto_format_frame(const ProtoHeader *hdr, const uint8_t *payload,
                        char *out, size_t outsz);

#endif /* PROTOCOL_H */