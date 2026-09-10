# Makefile for the C-Proxy Project

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDFLAGS = -lcrypto

# Executables
SERVER_EXEC = proxy_server
CLIENT_EXEC = test_client

# Source files
SERVER_SRCS = proxyserver.c cache.c utils.c proxy_parse.c
CLIENT_SRCS = test_client.c

# Object files
SERVER_OBJS = $(SERVER_SRCS:.c=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

# Default target: build both executables
all: $(SERVER_EXEC) $(CLIENT_EXEC)

# Linking the server executable
$(SERVER_EXEC): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $(SERVER_EXEC) $(SERVER_OBJS) $(LDFLAGS)

# Linking the client executable
$(CLIENT_EXEC): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $(CLIENT_EXEC) $(CLIENT_OBJS)

# Rule to compile .c files into .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
clean:
	rm -f $(SERVER_EXEC) $(CLIENT_EXEC) $(SERVER_OBJS) $(CLIENT_OBJS)

.PHONY: all clean