.PHONY: all clean

all: idma-test

idma-test: idma-test.c idma-proxy-userspace.h
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -o $@ idma-test.c

clean:
	rm -f idma-test
