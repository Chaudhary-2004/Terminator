CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -Iinclude -pthread
LDFLAGS = -lX11 -pthread

SOURCES = main.c src/gui.c src/shell.c src/parser.c src/tab_runtime.c
TARGET = terminator

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)