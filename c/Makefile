CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra
all: prism
src/prism.c: $(wildcard src/parts/*.c)
	./tools/amalgamate.sh
prism: src/prism.c src/main.c include/prism.h
	$(CC) $(CFLAGS) -Iinclude src/prism.c src/main.c -o prism -lm -lpthread
	cp prism prismflow
test: prism
	./tests/run.sh
clean:
	rm -f prism prismflow
.PHONY: all test clean
