# PongServer build file.
#   make        -> builds ./server
#   make clean  -> removes build output

CC      = gcc
# _POSIX_C_SOURCE: plain -std=c11 hides POSIX extras (sockets helpers, threads...).
CFLAGS  = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g -pthread
LDFLAGS = -pthread

TARGET  = server
SRCS    = main.c logger.c protocol.c sockopts.c session.c player.c lobby.c game.c
OBJS    = $(SRCS:.c=.o)
HDRS    = logger.h protocol.h sockopts.h session.h player.h lobby.h game.h

all: $(TARGET)

# Link: combine all object files into the final executable.
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Compile: turn each .c file into a .o (object) file.
# Listing $(HDRS) makes a header change trigger a rebuild too.
%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean