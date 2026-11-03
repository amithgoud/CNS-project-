# Makefile - Student Query Management System
#
#   make          build the server
#   make clean    remove build output
#
# Dependencies (Debian/Ubuntu):
#   sudo apt install build-essential libsqlite3-dev libcurl4-openssl-dev libcjson-dev

CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
LDLIBS   += -lsqlite3 -lcurl -lcjson

SERVER   := sqms_server
SRC      := $(wildcard src/*.c)
OBJ      := $(patsubst src/%.c,build/%.o,$(SRC))

.PHONY: all clean

all: $(SERVER)

$(SERVER): $(OBJ)
	$(CC) $(CFLAGS) -pthread -o $@ $^ $(LDLIBS)

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(SERVER)

-include $(OBJ:.o=.d)
