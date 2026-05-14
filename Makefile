CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS =

SRC = $(wildcard src/*.c)
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
OBJ = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC))

BIN = $(BUILD_DIR)/zvcs
WIN_BIN = $(BUILD_DIR)/zvcs.exe

all: $(BIN)

$(BIN): $(OBJ)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c
	mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR) src/*.o zvcs zvcs.exe

.PHONY: all clean

# Cross-compile for Windows with mingw-w64 (requires x86_64-w64-mingw32-gcc installed)
windows:
	mkdir -p $(BUILD_DIR)
	if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then \
		x86_64-w64-mingw32-gcc $(CFLAGS) -o $(WIN_BIN) $(SRC) $(LDFLAGS) && \
		echo "built $(WIN_BIN)"; \
	else \
		echo "x86_64-w64-mingw32-gcc not found; install mingw-w64"; exit 1; \
	fi
