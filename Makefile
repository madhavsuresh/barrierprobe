CC=gcc
FLAGS=-pthread -g -m64
all:
	$(CC) $(FLAGS) main.c  -o main
clean:
	rm main
