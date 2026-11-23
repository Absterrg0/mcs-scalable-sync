CC      ?= gcc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -pthread
LDFLAGS ?= -pthread

SRC_LOCK = src/lockbench.c src/locks.c
SRC_BAR  = src/barrierbench.c src/barriers.c
HDRS     = src/common.h src/locks.h src/barriers.h

all: lockbench barrierbench calibrate

calibrate: src/calibrate.c src/common.h
	$(CC) $(CFLAGS) -o $@ src/calibrate.c $(LDFLAGS)

lockbench: $(SRC_LOCK) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_LOCK) $(LDFLAGS)

barrierbench: $(SRC_BAR) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_BAR) $(LDFLAGS)

# ThreadSanitizer builds: data-race checking of every lock and barrier.
tsan: $(SRC_LOCK) $(SRC_BAR) $(HDRS)
	$(CC) -O1 -g -std=c11 -fsanitize=thread -pthread -o lockbench-tsan $(SRC_LOCK)
	$(CC) -O1 -g -std=c11 -fsanitize=thread -pthread -o barrierbench-tsan $(SRC_BAR)

# Quick correctness pass: every benchmark row checks mutual exclusion /
# barrier safety, and the binaries exit non-zero on any violation.
test: all
	./lockbench -l all -t 1,2,3,8,16,32 -d 0.2 -r 1 -c 2 > /dev/null && echo "locks: OK"
	./barrierbench -t 1,2,3,5,8 -e 20000 -r 1 > /dev/null && echo "barriers: OK"
	@# more threads than cores: spin barriers degrade to ~1 time slice/episode, so few episodes
	./barrierbench -t 13,24 -e 200 -r 1 > /dev/null && echo "barriers (oversubscribed): OK"

test-tsan: tsan
	./lockbench-tsan -l all -t 2,4,8 -d 0.2 -r 1 -c 1 > /dev/null && echo "locks (tsan): OK"
	./barrierbench-tsan -t 2,3,5,8 -e 2000 -r 1 > /dev/null && echo "barriers (tsan): OK"

clean:
	rm -f lockbench barrierbench calibrate lockbench-tsan barrierbench-tsan

.PHONY: all tsan test test-tsan clean
