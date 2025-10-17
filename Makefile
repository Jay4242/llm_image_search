CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g
LDFLAGS = -lraylib -lm -lpthread -ldl -lrt -lX11 -lcurl -ljansson

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
