obj-m += tcp_copa_auto.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_core test_v2

test: test_core.c test_v2.c copa_auto_v2.h copa_auto_core.h copa_auto_math.h
	$(CC) -O2 -Wall -Wextra -Werror -std=gnu11 -o test_core test_core.c -lm
	./test_core
	$(CC) -O2 -Wall -Wextra -Werror -std=gnu11 -o test_v2 test_v2.c
	./test_v2

.PHONY: all clean test
