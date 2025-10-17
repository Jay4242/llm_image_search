CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g
# ----------------------------------------------------------------------
# Platform‑specific link flags
# ----------------------------------------------------------------------
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)          # ---------- macOS ----------
    # Raylib on macOS needs the OpenGL / Cocoa / IOKit / CoreVideo frameworks
    LDFLAGS = -lraylib -lm -lpthread -lcurl -ljansson \
              -framework OpenGL -framework Cocoa \
              -framework IOKit -framework CoreVideo
else                               # ---------- Linux (fallback) ----------
    LDFLAGS = -lraylib -lm -lpthread -ldl -lrt -lX11 -lcurl -ljansson
endif
# ----------------------------------------------------------------------

SRC = main.c
OBJ = $(SRC:.c=.o)
TARGET = llm_image_search

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
