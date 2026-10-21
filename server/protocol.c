/*
 * PongProtocol v1: framing, encoding and validation (see protocol.h).
 */
#include "protocol.h"

#include <arpa/inet.h>   /* htons, ntohs */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>  /* send, recv */

/* ------------------------------------------------------------------ framing */

ProtoStatus send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR)   /* interrupted by a signal: just retry */
                continue;
            return PROTO_IO_ERROR;
        }
        sent += (size_t)n;
    }
    return PROTO_OK;
}

ProtoStatus recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0)
            return PROTO_CLOSED;  /* the peer closed (maybe in the middle of a message) */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return PROTO_IO_ERROR;
        }
        got += (size_t)n;
    }
    return PROTO_OK;
}

/* ----------------------------------------------------------- whole messages */

ProtoStatus proto_send(int fd, MsgType type, const uint8_t *payload, size_t len)
{
    if (len > PROTO_MAX_PAYLOAD)
        return PROTO_BAD_LENGTH;

    uint8_t frame[PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD];
    frame[0] = PROTO_VERSION;
    frame[1] = (uint8_t)type;

    /* Multi-byte integers go on the wire in network byte order (big-endian).
     * memcpy avoids writing a uint16_t through a possibly misaligned pointer. */
    uint16_t len_be = htons((uint16_t)len);
    memcpy(frame + 2, &len_be, sizeof len_be);

    if (len > 0)
        memcpy(frame + PROTO_HEADER_SIZE, payload, len);

    /* One send_all() for header + payload: they leave together in one segment. */
    return send_all(fd, frame, PROTO_HEADER_SIZE + len);
}

ProtoStatus proto_recv(int fd, ProtoHeader *hdr, uint8_t *payload, size_t max)
{
    /* 1. Read exactly 4 bytes: the header. */
    uint8_t raw[PROTO_HEADER_SIZE];
    ProtoStatus st = recv_all(fd, raw, sizeof raw);
    if (st != PROTO_OK)
        return st;

    uint16_t len_be;
    memcpy(&len_be, raw + 2, sizeof len_be);
    hdr->version = raw[0];
    hdr->type    = raw[1];
    hdr->length  = ntohs(len_be);   /* network -> host byte order */

    /* 2. NEVER trust what comes from the network: validate before reading more. */
    if (hdr->version != PROTO_VERSION)
        return PROTO_BAD_VERSION;
    if (hdr->length > PROTO_MAX_PAYLOAD || hdr->length > max)
        return PROTO_BAD_LENGTH;
    if (!proto_is_client_type(hdr->type))
        return PROTO_BAD_TYPE;

    /* 3. Read exactly `length` bytes: the payload (nothing to read if length == 0). */
    return recv_all(fd, payload, hdr->length);
}

/* ------------------------------------------------------------------ decoding */

static int is_nick_char(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

DecodeStatus proto_decode_register(const uint8_t *payload, size_t len,
                                   char *nick, char *email, const char **why)
{
    /* Layout first: nick_len:u8, nick[nick_len], email_len:u8, email[email_len].
     * The header's length must be EXACTLY 2 + nick_len + email_len.
     * Every index is checked against len before it is used. */
    if (len < 1) {
        *why = "empty payload";
        return DECODE_BAD_LAYOUT;
    }
    size_t nick_len = payload[0];
    if (len < 1 + nick_len + 1) {
        *why = "payload too short for the nickname";
        return DECODE_BAD_LAYOUT;
    }
    size_t email_len = payload[1 + nick_len];
    if (len != 2 + nick_len + email_len) {
        *why = "length does not match nick_len + email_len";
        return DECODE_BAD_LAYOUT;
    }

    const uint8_t *nick_p  = payload + 1;
    const uint8_t *email_p = payload + 2 + nick_len;

    /* Now the field rules (README 2.3.4). */
    if (nick_len < NICK_MIN || nick_len > NICK_MAX) {
        *why = "nickname length must be 1-16";
        return DECODE_BAD_FIELD;
    }
    for (size_t i = 0; i < nick_len; i++) {
        if (!is_nick_char(nick_p[i])) {
            *why = "nickname may only contain letters, digits, '_' and '-'";
            return DECODE_BAD_FIELD;
        }
    }
    if (email_len < EMAIL_MIN || email_len > EMAIL_MAX) {
        *why = "e-mail length must be 3-64";
        return DECODE_BAD_FIELD;
    }
    int has_at = 0;
    for (size_t i = 0; i < email_len; i++) {
        if (email_p[i] < 0x20 || email_p[i] > 0x7E) {
            *why = "e-mail must be printable ASCII";
            return DECODE_BAD_FIELD;
        }
        if (email_p[i] == '@')
            has_at = 1;
    }
    if (!has_at) {
        *why = "e-mail must contain '@'";
        return DECODE_BAD_FIELD;
    }

    /* Strings on the wire have no '\0'; add it so C can use them. */
    memcpy(nick, nick_p, nick_len);
    nick[nick_len] = '\0';
    memcpy(email, email_p, email_len);
    email[email_len] = '\0';
    return DECODE_OK;
}

DecodeStatus proto_decode_nick(const uint8_t *payload, size_t len, char *nick, const char **why)
{
    if (len < 1 || len != 1 + (size_t)payload[0]) {
        *why = "length does not match nick_len";
        return DECODE_BAD_LAYOUT;
    }
    size_t nick_len = payload[0];
    if (nick_len < NICK_MIN || nick_len > NICK_MAX) {
        *why = "nickname length must be 1-16";
        return DECODE_BAD_FIELD;
    }
    for (size_t i = 0; i < nick_len; i++) {
        if (!is_nick_char(payload[1 + i])) {
            *why = "nickname may only contain letters, digits, '_' and '-'";
            return DECODE_BAD_FIELD;
        }
    }
    memcpy(nick, payload + 1, nick_len);
    nick[nick_len] = '\0';
    return DECODE_OK;
}

/* ------------------------------------------------------------------- logging */

int proto_is_client_type(uint8_t type)
{
    return type == MSG_REGISTER || type == MSG_JOIN      ||
           type == MSG_INPUT    || type == MSG_QUIT      ||
           type == MSG_LIST     || type == MSG_PLAY_WITH;
}

const char *proto_type_name(uint8_t type)
{
    switch (type) {
    case MSG_REGISTER:    return "REGISTER";
    case MSG_JOIN:        return "JOIN";
    case MSG_INPUT:       return "INPUT";
    case MSG_QUIT:        return "QUIT";
    case MSG_LIST:        return "LIST";
    case MSG_PLAY_WITH:   return "PLAY_WITH";
    case MSG_REGISTER_OK: return "REGISTER_OK";
    case MSG_WAITING:     return "WAITING";
    case MSG_MATCH_START: return "MATCH_START";
    case MSG_STATE:       return "STATE";
    case MSG_GAME_OVER:   return "GAME_OVER";
    case MSG_USER_LIST:   return "USER_LIST";
    case MSG_ERROR:       return "ERROR";
    }
    return "UNKNOWN";
}

const char *proto_error_name(uint8_t code)
{
    switch (code) {
    case ERR_BAD_VERSION:   return "BAD_VERSION";
    case ERR_BAD_MESSAGE:   return "BAD_MESSAGE";
    case ERR_NICK_TAKEN:    return "NICK_TAKEN";
    case ERR_INVALID_STATE: return "INVALID_STATE";
    case ERR_INVALID_FIELD: return "INVALID_FIELD";
    case ERR_USER_NOT_AVAILABLE: return "USER_NOT_AVAILABLE";
    }
    return "UNKNOWN";
}

void proto_format_frame(const ProtoHeader *hdr, const uint8_t *payload,
                        char *out, size_t outsz)
{
    if (outsz == 0)
        return;

    /* Rebuild the bytes exactly as they travelled on the wire. */
    uint8_t frame[PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD];
    uint16_t len_be = htons(hdr->length);
    frame[0] = hdr->version;
    frame[1] = hdr->type;
    memcpy(frame + 2, &len_be, sizeof len_be);
    if (hdr->length > 0)
        memcpy(frame + PROTO_HEADER_SIZE, payload, hdr->length);
    size_t total = PROTO_HEADER_SIZE + hdr->length;

    /* Each byte takes 3 characters (" xx"); keep room for "..." and the '\0'. */
    size_t fit = (outsz > 4) ? (outsz - 4) / 3 : 0;
    size_t shown = total < fit ? total : fit;

    size_t o = 0;
    out[0] = '\0';
    for (size_t i = 0; i < shown; i++)
        o += (size_t)snprintf(out + o, outsz - o, i ? " %02x" : "%02x", frame[i]);
    if (shown < total)
        snprintf(out + o, outsz - o, "...");
}