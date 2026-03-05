CC := gcc
CFLAGS := -Wall -Wextra -std=c11 -g -fsanitize=address -Ilibs

all: server client

server: source/server.c source/markdown.c libs/markdown.h libs/command.h libs/document.h
	$(CC) $(CFLAGS) -o server source/server.c source/markdown.c -lpthread

client: source/client.c source/markdown.c libs/markdown.h libs/command.h libs/document.h
	$(CC) $(CFLAGS) -o client source/client.c source/markdown.c -lpthread

markdown.o: source/markdown.c libs/markdown.h libs/document.h
	$(CC) $(CFLAGS) -c source/markdown.c -o markdown.o

test: tests/test_markdown.c source/markdown.c libs/markdown.h libs/document.h
	$(CC) $(CFLAGS) -o test_markdown tests/test_markdown.c source/markdown.c -lpthread
	./test_markdown

clean:
	rm -f server client test_markdown markdown.o
	rm -f FIFO_C2S_* FIFO_S2C_*
	rm -f *.o core.* .DS_Store

.PHONY: all clean test
