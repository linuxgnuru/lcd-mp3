CC=gcc
CFLAGS=-c -Wall -g -O3
LDFLAGS=-lao -lmpg123 -lpthread -lm -lncurses -lwiringPi -lwiringPiDev 
BIN=lcd-mp3
SRC=$(BIN).c
OBJ=$(SRC:.c=.o)

all: $(SRC) $(BIN)

$(BIN):$(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@
.c.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf $(OBJ) $(BIN)
