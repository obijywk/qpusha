CFLAGS=-O3

all: benchmark

sha256.hex: sha256.s
	nodejs qpuasm.js sha256.s > sha256.hex

sha256.o: sha256.c sha256.hex
	gcc $(CFLAGS) -c sha256.c

mailbox.o: mailbox.c
	gcc $(CFLAGS) -c mailbox.c

benchmark: benchmark.c sha256.o mailbox.o
	gcc $(CFLAGS) -lrt -lm -o benchmark benchmark.c sha256.o mailbox.o

clean:
	rm *.o sha256.hex benchmark
