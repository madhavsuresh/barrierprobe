CC=gcc
FLAGS=-pthread  -m64 -g
all:
	$(CC) $(FLAGS) main.c  -o main
clean:
	rm main
