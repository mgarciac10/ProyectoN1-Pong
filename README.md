# Pong — Networked Game over PongProtocol

Project N1 · Internet: Architecture and Protocols

A client/server implementation of the classic **Pong** game. The server (**PongServer**, written in C with
Berkeley sockets and POSIX threads) keeps the authoritative game state for many simultaneous matches.
The clients (**PongClient**, Python + pygame) send the player's input and draw the state they receive.
Both sides talk through **PongProtocol**, a binary application-layer protocol designed for this project
and carried over TCP.

---

## 1. Introduction

*To be completed in the final phase.*

---

## 2. Development

### 2.1 Architecture

```
   PongClient A ──┐                    ┌──────────────── PongServer (C) ─────────────────┐
 (Python + pygame)│      TCP           │ main thread ── accept() loop                    │
                  ├───────────────────►│   └─ 1 client thread per connection             │
   PongClient B ──┘   PongProtocol     │ lobby ── pairs waiting players (FIFO)           │
                                       │   └─ 1 match thread per game (30 ticks/s)       │
                                       │ logger ── console + log file (thread-safe)      │
                                       └─────────────────────────────────────────────────┘
```

- **Main thread**: creates the listening socket and calls `accept()` forever. Every new connection gets its own thread.
- **Client thread**: blocks on `recv()`, decodes the client's messages and acts on them (registration, joining the queue, paddle input).
- **Match thread**: one per active game. Every tick it applies the inputs, moves the ball and paddles, detects collisions and goals, and sends the new state to both players.
- **Logger**: every relevant event (connections, registrations, matchmaking, goals, errors, disconnections) is printed to the console and appended to the log file under a mutex, so lines from different threads never mix.

**Why one thread per client?** `recv()` blocks: a thread waiting for one client cannot serve anyone else.
With a thread per client, each one waits only for its own client, so all players are served at the same
time. The alternative, a single thread multiplexing sockets with `select()`/`poll()`, scales better to
thousands of connections, but it is harder to follow. For the scale of this project (tens of players) threads
are enough, the assignment explicitly allows them, and each thread reads like a simple sequential program.

### 2.2 Transport: why TCP

PongProtocol runs over **TCP (`SOCK_STREAM`)** only, with one connection per client.

| Concern | With TCP | With UDP |
|---|---|---|
| Control messages (`REGISTER`, `MATCH_START`, `GAME_OVER`…) | Delivered reliably and in order. Losing one would break the state machine. | We would have to reimplement acknowledgements, retransmissions and ordering. |
| Game state (`STATE`, 30 per second) | A lost segment delays the following ones (*head-of-line blocking*). | A lost packet could simply be skipped. |
| NAT / firewalls | The client opens the connection, so it works behind home routers. | The server must learn and keep each client's UDP address and port alive. |
| Complexity | One socket per client, one code path. | Two sockets (or a custom reliability layer). |

Our messages are tiny (`STATE` is 14 bytes, about 420 B/s per player), and packet loss on today's
Internet is low, so head-of-line blocking is rarely noticeable. In exchange we get a much simpler and more
robust protocol. Two TCP options adapt it to a real-time game:

- **`TCP_NODELAY`**: disables Nagle's algorithm, which would otherwise hold small messages for a while to group them. Every `STATE`/`INPUT` leaves immediately.
- **Dead-peer detection**: see [Liveness](#237-procedure-rules).

### 2.3 PongProtocol v1 specification

#### 2.3.1 Service

PongProtocol lets a client:

1. **Register** a player profile (nickname + e-mail) on the server.
2. **Join** the matchmaking queue and be paired with the next waiting player (first come, first served).
3. **Play** a match: the client sends its paddle *intention* (up / down / stop); the server, which is the only authority on the game state, simulates the match and broadcasts the state to both players 30 times per second.
4. Be **notified** when the match ends (a player reached 5 points, or the opponent left), and **join again** without reconnecting.
5. **Leave** at any time.

#### 2.3.2 Encoding rules

- Every message is a **4-byte header** followed by a **payload** of `length` bytes (0–255).
- All multi-byte integers are unsigned and in **network byte order (big-endian)**.
- Strings are **length-prefixed**: one `u8` with the length, then the bytes. There is no terminating `\0`. Characters are ASCII.
- Types: `u8` = 1 byte, `u16` = 2 bytes.

**Header**

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | u8 | `version` | Protocol version. Always `1`. |
| 1 | u8 | `type` | Message type (see vocabulary). |
| 2 | u16 | `length` | Payload size in bytes. Must be ≤ 255. |

**Framing.** TCP is a byte stream: one `recv()` can return part of a message, or several messages glued
together. The receiver always reads **exactly 4 bytes** (the header), then **exactly `length` bytes** (the
payload), looping on `recv()` until each part is complete.

#### 2.3.3 Message vocabulary

Types with the high bit set (`0x80`) are sent by the server.

| Type | Name | Direction | Payload size | Meaning |
|---|---|---|---|---|
| `0x01` | `REGISTER` | C → S | variable | Create my player profile. |
| `0x02` | `JOIN` | C → S | 0 | Put me in the matchmaking queue. |
| `0x03` | `INPUT` | C → S | 1 | My paddle direction changed. |
| `0x04` | `QUIT` | C → S | 0 | I am leaving; close the connection. |
| `0x81` | `REGISTER_OK` | S → C | 0 | Registration accepted. |
| `0x82` | `WAITING` | S → C | 0 | You are queued; waiting for an opponent. |
| `0x83` | `MATCH_START` | S → C | variable | A match begins: your side and your opponent. |
| `0x84` | `STATE` | S → C | 10 | Current positions and score (every tick). |
| `0x85` | `GAME_OVER` | S → C | 2 | The match ended. |
| `0xFF` | `ERROR` | S → C | 1 | Your last message was rejected. |

#### 2.3.4 Payload layouts

**`REGISTER` (0x01)**

| Offset | Size | Field | Rule |
|---|---|---|---|
| 0 | u8 | `nick_len` | 1–16 |
| 1 | `nick_len` | `nick` | letters, digits, `_` or `-` |
| 1 + n | u8 | `email_len` | 3–64 |
| 2 + n | `email_len` | `email` | printable ASCII, contains `@` |

The header's `length` must be exactly `2 + nick_len + email_len`.

**`INPUT` (0x03)**

| Offset | Size | Field | Values |
|---|---|---|---|
| 0 | u8 | `dir` | `0` = stop, `1` = up, `2` = down |

**`MATCH_START` (0x83)**

| Offset | Size | Field | Values |
|---|---|---|---|
| 0 | u8 | `side` | `0` = left paddle, `1` = right paddle |
| 1 | u8 | `opp_len` | 1–16 |
| 2 | `opp_len` | `opp_nick` | opponent's nickname |

**`STATE` (0x84)**: always 10 bytes.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | u16 | `ball_x` | ball's top-left corner, x |
| 2 | u16 | `ball_y` | ball's top-left corner, y |
| 4 | u16 | `left_y` | left paddle's top edge, y |
| 6 | u16 | `right_y` | right paddle's top edge, y |
| 8 | u8 | `score_l` | left player's score |
| 9 | u8 | `score_r` | right player's score |

**`GAME_OVER` (0x85)**

| Offset | Size | Field | Values |
|---|---|---|---|
| 0 | u8 | `winner` | `0` = left, `1` = right |
| 1 | u8 | `reason` | `0` = a player reached the winning score, `1` = the opponent left |

**`ERROR` (0xFF)**

| Offset | Size | Field | Values |
|---|---|---|---|
| 0 | u8 | `code` | see error codes |

`JOIN`, `QUIT`, `REGISTER_OK` and `WAITING` have an empty payload (`length` = 0).

#### 2.3.5 Error codes

| Code | Name | When | Connection |
|---|---|---|---|
| 1 | `BAD_VERSION` | `version` ≠ 1 | **closed** |
| 2 | `BAD_MESSAGE` | unknown `type`, `length` > 255, or `length` doesn't match the message layout | **closed** |
| 3 | `NICK_TAKEN` | another connected player already uses that nickname | stays open (client may retry) |
| 4 | `INVALID_STATE` | the message isn't allowed in the client's current state | stays open (message ignored) |
| 5 | `INVALID_FIELD` | a field breaks its rule (e.g. empty nick, e-mail without `@`, `dir` > 2) | stays open (message ignored) |

Codes 1 and 2 close the connection because, after a malformed header, the receiver can no longer tell
where the next message starts: the byte stream is out of sync.

#### 2.3.6 Game constants

Both applications use these fixed values. They are part of the specification and are never sent.

| Constant | Value |
|---|---|
| Field size | 800 × 600 units. Origin at the top-left corner, x grows right, y grows down. |
| Paddle size | 10 × 80. Left paddle at x = 20, right paddle at x = 770. |
| Ball size | 10 × 10 |
| Tick rate | 30 ticks/s: one `STATE` to each player every ~33 ms |
| Winning score | 5 |

Positions are integers, so we never have to serialize floating-point numbers. The server is free to
simulate internally with more precision.

#### 2.3.7 Procedure rules

**States (server's view of each client)**

| State | Allowed client messages | Anything else |
|---|---|---|
| `CONNECTED` | `REGISTER`, `QUIT` | `ERROR(4)` |
| `REGISTERED` | `JOIN`, `QUIT` | `ERROR(4)` |
| `WAITING` | `QUIT` | `ERROR(4)` |
| `PLAYING` | `INPUT`, `QUIT` | `ERROR(4)` |

```mermaid
stateDiagram-v2
    [*] --> CONNECTED: TCP connection accepted
    CONNECTED --> CONNECTED: REGISTER invalid or nick taken / ERROR(3 or 5)
    CONNECTED --> REGISTERED: REGISTER valid / REGISTER_OK
    REGISTERED --> WAITING: JOIN, nobody waiting / WAITING
    REGISTERED --> PLAYING: JOIN, someone waiting / MATCH_START
    WAITING --> PLAYING: opponent joins / MATCH_START
    PLAYING --> REGISTERED: match ends / GAME_OVER
    CONNECTED --> [*]: QUIT, disconnect or ERROR(1 or 2)
    REGISTERED --> [*]: QUIT or disconnect
    WAITING --> [*]: QUIT or disconnect
    PLAYING --> [*]: QUIT or disconnect, opponent gets GAME_OVER(reason 1)
```

**Rules**

1. **Registration first.** A client must register before anything else. The nickname must be unique among *connected* players. Profiles live in memory and are released when the client disconnects.
2. **Matchmaking.** On `JOIN`, if another player is waiting, both are paired at once and receive `MATCH_START`. Otherwise the client receives `WAITING` and is paired with the next player who joins.
3. **Authoritative server.** `INPUT` only changes the paddle's *direction*. The server moves the paddle at its own fixed speed, keeps it inside the field, moves the ball, detects collisions and goals, and keeps the score. Clients never send positions.
4. **State broadcast.** During a match, the server sends `STATE` to both players every tick. The client sends `INPUT` only when the direction **changes** (not every frame).
5. **End of match.** When a player reaches 5 points, both receive `GAME_OVER(winner, 0)` and return to `REGISTERED`. They may `JOIN` again on the same connection.
6. **Opponent leaves.** If a player sends `QUIT`, disconnects, or is detected as dead during a match, the opponent receives `GAME_OVER(opponent's side, 1)` and returns to `REGISTERED`.
7. **Late input.** An `INPUT` received outside `PLAYING` is **silently discarded**, not answered with an error. It may legitimately cross a `GAME_OVER` that is already on its way to the client.
8. **Leaving.** After `QUIT` the server closes the connection. A player who quits while `WAITING` is removed from the queue.
9. **Errors.** See error codes: protocol violations (1, 2) close the connection; semantic errors (3, 4, 5) are reported and the message is ignored.

**Liveness (dead-peer detection)**

A client can disappear without closing its connection: it loses Wi-Fi, switches networks, or its machine
crashes. Since TCP sends nothing on an idle connection, the server would wait in `recv()` forever. A
"ghost" player would then stay in the queue, or freeze a match. PongProtocol has no heartbeat message.
Instead, the server configures every client socket with:

| Socket option | Value | Covers |
|---|---|---|
| `SO_KEEPALIVE` + `TCP_KEEPIDLE` / `TCP_KEEPINTVL` / `TCP_KEEPCNT` | 5 s / 2 s / 3 probes | idle connections (registered or waiting players): the kernel probes a silent peer |
| `TCP_USER_TIMEOUT` | 10 s | active connections (during a match): data sent but not acknowledged for 10 s aborts the connection |

A dead client is detected within about 15 s. Its `recv()` then fails, and the server treats it exactly like
a disconnection (rules 6 and 8). Trade-off: these options are part of the operating system (Linux), not of
the protocol messages. The protocol stays smaller, but the behaviour depends on the server's socket
configuration.

#### 2.3.8 Sequence diagrams

**Normal flow: register, match, game over, leave**

```mermaid
sequenceDiagram
    participant A as Client A
    participant S as Server
    participant B as Client B
    A->>S: REGISTER("ana", "ana@x.co")
    S-->>A: REGISTER_OK
    A->>S: JOIN
    S-->>A: WAITING
    B->>S: REGISTER("bob", "bob@y.co")
    S-->>B: REGISTER_OK
    B->>S: JOIN
    S-->>A: MATCH_START(side=0, "bob")
    S-->>B: MATCH_START(side=1, "ana")
    loop every tick (~33 ms)
        S-->>A: STATE
        S-->>B: STATE
    end
    A->>S: INPUT(up)
    B->>S: INPUT(down)
    Note over S: a player reaches 5 points
    S-->>A: GAME_OVER(winner=0, reason=0)
    S-->>B: GAME_OVER(winner=0, reason=0)
    A->>S: QUIT
    Note over S: closes A's connection
```

**Nickname already taken, then retry**

```mermaid
sequenceDiagram
    participant C as Client
    participant S as Server
    C->>S: REGISTER("ana", "ana2@x.co")
    S-->>C: ERROR(3 NICK_TAKEN)
    C->>S: REGISTER("ana_2", "ana2@x.co")
    S-->>C: REGISTER_OK
```

**Opponent disappears mid-match**

```mermaid
sequenceDiagram
    participant A as Client A
    participant S as Server
    participant B as Client B
    S-->>A: STATE
    S--xB: STATE (B lost its network)
    Note over S,B: no ACK from B for 10 s, TCP_USER_TIMEOUT aborts the connection
    Note over S: B's recv() fails, B is treated as disconnected
    S-->>A: GAME_OVER(winner=0, reason=1)
    Note over A: back to REGISTERED, may JOIN again
```

#### 2.3.9 Byte-level example

`REGISTER("ana", "ana@x.co")` is 17 bytes on the wire:

```
01             version   = 1
01             type      = 0x01 REGISTER
00 0D          length    = 13 (big-endian) = 1 + 3 + 1 + 8
03             nick_len  = 3
61 6E 61       nick      = "ana"
08             email_len = 8
61 6E 61 40 78 2E 63 6F   email = "ana@x.co"
```

### 2.4 Logger

Every line has the format `YYYY-MM-DD HH:MM:SS [LEVEL] message`, with `LEVEL` ∈ {`INFO`, `WARN`, `ERROR`}.
Messages related to a client start with its `ip:port`. The same line goes to the console and to the log file.

```
2026-10-10 00:42:39 [INFO] 127.0.0.1:54570 connected
2026-10-10 00:42:41 [ERROR] 127.0.0.1:59724 send: Connection reset by peer
```

### 2.5 Build and run

**Server** (Linux, gcc, make):

```bash
cd server
make
./server <PORT> <LogFile>      # e.g. ./server 5000 pong.log
```

**Client**: *to be completed (Phase 7).*

---

## 3. Conclusions

*To be completed in the final phase.*

---

## 4. References

- B. Hall, *Beej's Guide to Network Programming*. https://beej.us/guide/bgnet/
- B. Hall, *Beej's Guide to C Programming*. https://beej.us/guide/bgc/
- GeeksforGeeks, *TCP Server-Client implementation in C*. https://www.geeksforgeeks.org/tcp-server-client-implementation-in-c/
- W. Eddy, *RFC 9293: Transmission Control Protocol (TCP)*, IETF, 2022.
- *Pong*. Wikipedia. https://en.wikipedia.org/wiki/Pong
- Linux man pages: `socket(7)`, `tcp(7)`, `pthreads(7)`.
