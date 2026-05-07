.PHONY: all clean

all: idma-legacy-test

idma-legacy-test: idma-legacy-test.c idma-legacy-ioctl.h
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -o $@ idma-legacy-test.c

clean:
	rm -f idma-legacy-test
