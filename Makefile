# Makefile - Student Query Management System
#
#   make              build the server (sqms_server) and the CLI client (sqms_client)
#   make HTTPS=1      same, with HTTPS support (needs libssl-dev); run "make clean" first
#                     when switching between HTTPS=1 and a normal build
#   make cert         create a self-signed certificate in certs/ for HTTPS
#   make test         run the concurrency test against a running server
#   make clean        remove build output
#
# Dependencies (Debian/Ubuntu):
#   sudo apt install build-essential libsqlite3-dev libcurl4-openssl-dev libcjson-dev
#   (HTTPS bonus: libssl-dev)

CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
LDLIBS   += -lsqlite3 -lcurl -lcjson

ifeq ($(HTTPS),1)
CPPFLAGS += -DSQMS_TLS
LDLIBS   += -lssl -lcrypto
endif

SERVER   := sqms_server
CLIENT   := sqms_client
SRC      := $(wildcard src/*.c)
OBJ      := $(patsubst src/%.c,build/%.o,$(SRC))

.PHONY: all clean cert test

all: $(SERVER) $(CLIENT)

$(SERVER): $(OBJ)
	$(CC) $(CFLAGS) -pthread -o $@ $^ $(LDLIBS)

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c $< -o $@

# The client is a separate program with no library dependencies.
$(CLIENT): client/sqms_client.c
	$(CC) -D_POSIX_C_SOURCE=200809L $(CFLAGS) -o $@ $<

build:
	mkdir -p build

cert:
	mkdir -p certs
	openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
		-keyout certs/server.key -out certs/server.crt \
		-subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
	chmod 600 certs/server.key

test: $(CLIENT)
	bash tests/load_test.sh 20

clean:
	rm -rf build $(SERVER) $(CLIENT)

-include $(OBJ:.o=.d)
