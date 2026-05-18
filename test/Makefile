CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2 -I../src/c
LDFLAGS = -lm

sun_test: sun_test.c ../src/c/sun.c ../src/c/sun.h
	$(CC) $(CFLAGS) -o $@ sun_test.c ../src/c/sun.c $(LDFLAGS)

.PHONY: test clean
test: sun_test
	./sun_test

clean:
	rm -f sun_test
