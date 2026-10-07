CC     ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow

all: bserve bcurl

bserve: src/bserve.c src/proto.c src/proto.h
	$(CC) $(CFLAGS) -o $@ src/bserve.c src/proto.c

bcurl: src/bcurl.c src/proto.c src/proto.h
	$(CC) $(CFLAGS) -o $@ src/bcurl.c src/proto.c

test: all
	python3 tests/test_bhttp.py -v

clean:
	rm -rf bserve bcurl *.dSYM tests/__pycache__

.PHONY: all test clean
