CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
LDLIBS   = -lz -lm -lpthread

SRC = src/main.c src/game.c src/render.c src/term.c src/audio.c
OBJ = $(SRC:.c=.o)
BIN = kilix-fishtank

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/fishtank.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/render.o: src/font8x16.h src/embedded_assets.h

test: $(BIN)
	./$(BIN) --selftest 1337 7200
	./$(BIN) --render-test 42

clean:
	rm -f $(OBJ) $(BIN) render_*.ppm render_*.png

.PHONY: all test clean
