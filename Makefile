
CFLAGS = -O2 -Wall -Wextra -pedantic 
SSL_FLAGS = -lssl -lcrypto

all :main  run

main: main.c 
	gcc $(CFLAGS) -o main main.c $(SSL_FLAGS)

run:
	./main http://example.org/path

clean:
	rm -rf main