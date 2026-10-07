# PongServer build file.
#   make        -> builds ./server
#   make clean  -> removes build output

CC      = gcc
# _POSIX_C_SOURCE: plain -std=c11 hides POSIX extras (sockets helpers, threads...).
CFLAGS  = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g -pthread
LDFLAGS = -pthread

TARGET  = server
SRCS    = main.c
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

# Link: combine all object files into the final executable.
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Compile: turn each .c file into a .o (object) file.
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
