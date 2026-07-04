
CFLAGS = -O2 -Wall -Wextra -pedantic

all :main  run

main: main.c 
	gcc $(CFLAGS) -o main main.c

run:
	./main http://example.org/path

clean:
	rm -rf main