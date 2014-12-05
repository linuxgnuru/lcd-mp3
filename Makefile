lcd-mp3:lcd-mp3.o
	gcc -Wall -g -O0 lcd-mp3.c -o lcd-mp3 -lao -lmpg123 -lpthread -lm -lncurses -lwiringPi -lwiringPiDev 
