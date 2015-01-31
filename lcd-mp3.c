// TODO

/*
 *	lcd-mp3
 *
 *	John Wiggins (jcwiggi@gmail.com)
 *
 *	mp3 player with output to a 16x2 lcd display for song information
 *
 *	Portions of this code were borrowed from MANY other projects including (but limited to)
 *	- wiringPi example code for lcd.c
 *	- http://hzqtc.github.io/2012/05/play-mp3-with-libmpg123-and-libao.html
 *	- http://www.arduino.cc/en/Tutorial/Debounce
 *	- many thanks to those who helped me out at StackExchange
 *	  (http://raspberrypi.stackexchange.com/)
 *
 *      requires:
 *		ncurses
 *		pthread
 *		wiringPi
 *		ao
 *		mpg123
 *
 *
 *	31-01-2015	Setting up for final project; might need to fork this into a LCD only.
 *			Things to be done:
 *				- Change -dir and -songs to -d and -s and --dir= and --songs=
 *				- FIXME song file names that have a space kill this... find out why
 *				- On startup, (assuming the /etc/fstab has already mounted a USB drive (/dev/sda1)
 *				  to /MUSIC) check /MUSIC and load in all music.
 *				- if Quit button is pressed, display "QUIT - Shutdown" and "PAUSE - Cancel"
 *	27-01-2015	Removed the LEDs from below...
 *	25-01-2015	Added some LEDs to also indicate when paused and if info is on or not
 *	07-12-2014	Added the PAUSED text to the LCD when paused and disabled all other buttons while paused.
 *			FIXME need to allow user to quit program while paused. Removed unused/debug code
 *	06-12-2014	see if we can do something about the song being paused while next/prev/quit/info
 *			buttons are pressed
 *	05-12-2014	Minor changes; removed btnFlag
 *	02-12-2014	Removed everything regarding debouncing function; going back to old delay setup
 *	02-12-2014	Removed debouncing function and added it manually; having too many odd
 *			struct errors
 *	29-11-2014	Tons of modifications:
 *			- added a mount/unmount USB (assuming /dev/sda1) option
 *			- added more argument options such as whole dir, and mount usb
 *			- worked with another way to debounce buttons
 *	28-11-2014	added some help
 *	25-11-2014	attempt to add button support
 *	22-11-2014	moved from an array of songs to a linked list (playlist) of songs.
 *	18-11-2014	lots of re-work, added more info to curses display and the ability to
 *			swap what is shown on the second row; album vs artist
 *	17-11-2014	attempt to add previous song
 *	17-11-2014	added quit
 *	16-11-2014	added skip song / next song
 *	14-11-2014	added ability to pause thread/song; using ncurses, got keboard commands
 *	12-11-2014	added playback of multiple songs
 *	10-11-2014	added ID3 parsing of MP3 file
 *	04-11-2014	made the song playing part a thread
 *	02-11-2014	able to play a mp3 file using mpg123 and ao libraries
 *	28-10-2014	worked on scrolling text on lcd
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <libgen.h>

// for mounting
#include <sys/mount.h>
#include <dirent.h> 

#include <wiringPi.h>
#include <lcd.h>

#include "lcd-mp3.h"

// --------- BEGIN USER MODIFIABLE VARS ---------

// GPIO pins (using wiringPi numbers)
#define playButtonPin  0 // GPIO 0, BCM 17
#define prevButtonPin  1 // GPIO 1, BCM 18
#define nextButtonPin  2 // GPIO 2, BCM 27
#define infoButtonPin  5 // GPIO 5, BCM 24
#define quitButtonPin  7 // GPIO 7, BCM  4

#define BTN_DELAY 30

// --------- END USER MODIFIABLE VARS ---------

const int buttonPins[] = {
	playButtonPin,
	prevButtonPin,
	nextButtonPin,
	infoButtonPin,
	quitButtonPin
	};

// linked list / playlist functions

int playlist_init(playlist_t *playlistptr)
{
  *playlistptr = NULL;
  return 1;
}

int playlist_add_song(int index, void *songptr, playlist_t *playlistptr)
{
  playlist_node_t *cur, *prev, *new;
  int found = FALSE;

  for (cur = prev = *playlistptr; cur != NULL; prev = cur, cur = cur->nextptr)
  {
    if (cur->index == index)
    {
      free(cur->songptr);
      cur->songptr = songptr;
      found = TRUE;
      break;
    }
    else if (cur->index > index)
      break;
  }
  if (!found)
  {
    new = (playlist_node_t *)malloc(sizeof(playlist_node_t));
    new->index = index;
    new->songptr = songptr;
    new->nextptr = cur;
    if (cur == *playlistptr)
      *playlistptr = new;
    else
      prev->nextptr = new;
  }
  return 1;
}

int playlist_get_song(int index, void **songptr, playlist_t *playlistptr)
{
  playlist_node_t *cur, *prev;

  // Initialize to "not found"
  *songptr = NULL;
  // Look through index for our entry
  for (cur = prev = *playlistptr; cur != NULL; prev = cur, cur = cur->nextptr)
  {
    if (cur->index == index)
    {
      *songptr = cur->songptr;
      break;
    }
    else if (cur->index > index)
      break;
  }
  return 1;
}

// Mounting function; might use it in the future

// mount (if cmd == 1, do not attempt to unmount)
int mountToggle(int cmd, char *dir_name)
{
	if (mount("/dev/sda1", dir_name, "vfat", MS_RDONLY | MS_SILENT, "") == -1)
	{
		// if it is already mounted; then unmount it.
		if (errno == EBUSY && cmd == 2)
		{
			umount2("/MUSIC", MNT_FORCE);
			return UNMOUNTED;
		}
		// filesystem is already mounted so just return as mounted.
		else if (errno == EBUSY && cmd != 2)
			return MOUNTED;
		else
			return MOUNT_ERROR;
	}
	else
		return MOUNTED;
}

// if USB has been mounted, load in songs.
playlist_t reReadPlaylist(char *dir_name)
{
	int index;
	char *string;
	playlist_t new_playlist;
	DIR *d;
	struct dirent *dir;

	index = 1;
	playlist_init(&new_playlist);
	d = opendir(dir_name);
	if (d)
	{
		while ((dir = readdir(d)) != NULL)
		{
			if (dir->d_type == 8)
			{
				string = malloc(MAXDATALEN);
				if (string == NULL)
					perror("malloc");
				strcpy(string, dir_name);
				strcat(string, "/");
				strcat(string, dir->d_name);
				playlist_add_song(index++, string, &new_playlist);
			}
		}
	}
	closedir(d);
	pthread_mutex_lock(&cur_song.pauseMutex);
	num_songs = index;
	pthread_mutex_unlock(&cur_song.pauseMutex);
	return new_playlist;
}

// pthread stuff
void nextSong()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = NEXT;
	cur_song.song_over = TRUE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void prevSong()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = PREV;
	cur_song.song_over = TRUE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void quitMe()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = QUIT;
	cur_song.song_over = TRUE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void pauseMe()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = PAUSE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}


void playMe()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = PLAY;
	pthread_cond_broadcast(&cur_song.m_resumeCond);
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void checkPause()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	while (cur_song.play_status == PAUSE)
		pthread_cond_wait(&cur_song.m_resumeCond, &cur_song.pauseMutex);
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

// ID3 stuff

// Split up a number of lines separated by \n, \r, both or just zero byte
//   and print out each line with specified prefix.
void make_id(mpg123_string *inlines, int type)
{
	size_t i;
	int hadcr = 0, hadlf = 0;
	char *lines = NULL;
	char *line  = NULL;
	size_t len = 0;
	char tmp_name[100];

	if (inlines != NULL && inlines->fill)
	{
		lines = inlines->p;
		len = inlines->fill;
	}
	else
		return;
	line = lines;
	for (i = 0; i < len; ++i)
	{
		if (lines[i] == '\n' || lines[i] == '\r' || lines[i] == 0)
		{
			// saving, changing, restoring a byte in the data
			char save = lines[i];
			if (save == '\n')
				++hadlf;
			if (save == '\r')
				++hadcr;
			if ((hadcr || hadlf) && hadlf % 2 == 0 && hadcr % 2 == 0)
				line = "";
			if (line)
			{
				lines[i] = 0;
				strncpy(tmp_name, line, 100);
				line = NULL;
				lines[i] = save;
			}
		}
		else
		{
			hadlf = hadcr = 0;
			if (line == NULL)
				line = lines + i;
		}
	}
	switch (type)
	{
		case  TITLE: strcpy(cur_song.title,  tmp_name); break;
		case ARTIST: strcpy(cur_song.artist, tmp_name); break;
		case  GENRE: strcpy(cur_song.genre,  tmp_name); break;
		case  ALBUM: strcpy(cur_song.album,  tmp_name); break;
	}
}

int id3_tagger()
{
	int meta;
	mpg123_handle* m;
	mpg123_id3v1 *v1;
	mpg123_id3v2 *v2;

	// ID3 tag info for the song
	mpg123_init();
	m = mpg123_new(NULL, NULL);
	if (mpg123_open(m, cur_song.filename) != MPG123_OK)
	{
		fprintf(stderr, "Cannot open %s: %s\n", cur_song.filename, mpg123_strerror(m));
		return 1;
	}
	mpg123_scan(m);
	meta = mpg123_meta_check(m);
	if (meta & MPG123_ID3 && mpg123_id3(m, &v1, &v2) == MPG123_OK)
	{
		make_id(v2->title, TITLE);
		make_id(v2->artist, ARTIST);
		make_id(v2->album, ALBUM);
		make_id(v2->genre, GENRE);
	}
	else
	{
		sprintf(cur_song.title, "UNKNOWN");
		sprintf(cur_song.artist, "UNKNOWN");
		sprintf(cur_song.album, "UNKNOWN");
		sprintf(cur_song.genre, "UNKNOWN");
	}
	// if there is no title to be found, set title to the song file name.
	if (strlen(cur_song.title) == 0)
		strcpy(cur_song.title, cur_song.base_filename);
	if (strlen(cur_song.artist) == 0)
		sprintf(cur_song.artist, "UNKNOWN");
	if (strlen(cur_song.album) == 0)
		sprintf(cur_song.album, "UNKNOWN");
	// set the second row to be the artist by default.
	strcpy(cur_song.second_row_text, cur_song.artist);
	mpg123_close(m);
	mpg123_delete(m);
	mpg123_exit();
	// the following two lines are just to see when the scrolling should pause
	strncpy(cur_song.scroll_firstRow, cur_song.title, 15);
	strncpy(cur_song.scroll_secondRow, cur_song.second_row_text, 16);
	return 0;
}

int printLcdFirstRow()
{
	int flag = TRUE;
	// have to set to 15 because of music note
	if (strlen(cur_song.title) < 15)
	{
		lcdCharDef(lcdHandle, 2, musicNote);
		lcdPosition(lcdHandle, 0, 0);
		lcdPutchar(lcdHandle, 2);
		lcdPosition(lcdHandle, 1, 0);
		lcdPuts(lcdHandle, cur_song.title);
		flag = FALSE;
	}
	return flag;
}

int printLcdSecondRow()
{
	int flag = TRUE;
	if (strlen(cur_song.second_row_text) < 16)
	{
		lcdPosition(lcdHandle, 0, 1);
		lcdPuts(lcdHandle, cur_song.second_row_text);
		flag = FALSE;
	}
	return flag;
}

int usage(const char *progName)
{
	//"-usb [mount] "
	fprintf(stderr, "Usage: %s [OPTION] \n"
		"-pins (shows what pins to use for buttons) \n"
		"-dir [dir] \n"
		"-songs [MP3 files]\n", progName);
	return EXIT_FAILURE;
}

void scrollMessage_firstRow(void)
{
	char buf[32];
	static int position = 0;
	static int timer = 0;
	int width = 15;
	char my_songname[MAXDATALEN];

	strcpy(my_songname, spaces);
	strncat(my_songname, cur_song.title, strlen(cur_song.title));
	strcat(my_songname, spaces);
	my_songname[strlen(my_songname) + 1] = 0;
	if (millis() < timer)
		return;
	timer = millis() + 200;
	strncpy(buf, &my_songname[position], width);
	buf[width] = 0;
	lcdCharDef(lcdHandle, 2, musicNote);
	lcdPosition(lcdHandle, 0, 0);
	lcdPutchar(lcdHandle, 2);
	lcdPosition(lcdHandle, 1, 0);
	lcdPuts(lcdHandle, buf);
	position++;
	// pause briefly when text reaches begining line before continuing
	if (strcmp(buf, cur_song.scroll_firstRow) == 0)
		delay(1500);
	if (position == (strlen(my_songname) - width))
		position = 0;
}

void scrollMessage_secondRow(void)
{
	char buf[32];
	static int position = 0;
	static int timer = 0;
	int width = 16;
	char my_string[MAXDATALEN];

	strcpy(my_string, spaces);
	strncat(my_string, cur_song.second_row_text, strlen(cur_song.second_row_text));
	strcat(my_string, spaces);
	my_string[strlen(my_string) + 1] = 0;
	if (millis() < timer)
		return;
	timer = millis() + 200;
	strncpy(buf, &my_string[position], width);
	buf[width] = 0;
	lcdPosition(lcdHandle, 0, 1);
	lcdPuts(lcdHandle, buf);
	position++;
	// pause briefly when text reaches begining line before continuing
	if (strcmp(buf, cur_song.scroll_secondRow) == 0)
		delay(1500);
	if (position == (strlen(my_string) - width))
		position = 0;
}

// The actual thing that plays the song
void *play_song(void *arguments)
{
	struct song_info *args = (struct song_info *)arguments;
	mpg123_handle *mh;
	mpg123_pars *mpar;
	unsigned char *buffer;
	size_t buffer_size;
	size_t done;
	int err;

	int driver;
	ao_device *dev;
	ao_sample_format format;
	int channels, encoding;
	long rate;

	ao_initialize();
	driver = ao_default_driver_id();
	mpg123_init();
	// try to not show error messages
	mh = mpg123_new(NULL, &err);
	mpar = mpg123_new_pars(&err);
	mpg123_par(mpar, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
	mh = mpg123_parnew(mpar, NULL, &err);
	buffer_size = mpg123_outblock(mh);
	buffer = (unsigned char*) malloc(buffer_size * sizeof(unsigned char));
	// open the file and get the decoding format
	mpg123_open(mh, args->filename);
	mpg123_getformat(mh, &rate, &channels, &encoding);
	// set the output format and open the output device
	format.bits = mpg123_encsize(encoding) * 8;
	format.rate = rate;
	format.channels = channels;
	format.byte_format = AO_FMT_NATIVE;
	format.matrix = 0;
	dev = ao_open_live(driver, &format, NULL);
	// decode and play
	while (mpg123_read(mh, buffer, buffer_size, &done) == MPG123_OK)
	{
		checkPause();
		ao_play(dev, buffer, done);
		// stop playing if the user pressed quit, next, or prev buttons
		if (cur_song.play_status == QUIT || cur_song.play_status == NEXT || cur_song.play_status == PREV)
			break;
	}
	// clean up
	free(buffer);
	ao_close(dev);
	mpg123_close(mh);
	mpg123_delete(mh);
	mpg123_exit();
	ao_shutdown();
	pthread_mutex_lock(&(cur_song.writeMutex));
	args->song_over = TRUE;
	// only set the status to play if the song finished normally
	if (cur_song.play_status != QUIT && cur_song.play_status != NEXT && cur_song.play_status != PREV)
		args->play_status = PLAY;
	cur_status.song_over = TRUE;
	pthread_mutex_unlock(&(cur_song.writeMutex));
}

int main(int argc, char **argv)
{
	pthread_t song_thread;
	playlist_t cur_playlist;
	char *basec, *bname;
	char *string;
	char pause_text[MAXDATALEN];
	char lcd_clear[] = "                ";
	int index;
	int song_index;
	int key;
	int i;
	int LCD_ONLY = FALSE;
	//int mountFlag = UNMOUNTED;
	int useButtonFlag = TRUE;
	int scroll_firstRow_flag, scroll_secondRow_flag;
	int buttonState;
	int btnCtr;

	// Initializations
	playlist_init(&cur_playlist);
	cur_song.song_over = FALSE;
	scroll_firstRow_flag = scroll_secondRow_flag = FALSE;
	if (argc > 1)
	{
		if (strcmp(argv[1], "-pins") == 0)
		{
			printf("Pins for buttons:\nFunction\twiringPi\tBCM\n"
			       "--------\t--------\t---\n"
			       "Play    \t0       \t17\n"
			       "Prev    \t1       \t18\n"
			       "Next    \t2       \t27\n"
			       "Info    \t5       \t25\n"
			       "Quit    \t7       \t4\n\n"
			       return 1;
		}
		else if (strcmp(argv[1], "-songs") == 0)
		{
			for (index = 2; index < argc; index++)
			{
				string = malloc(MAXDATALEN);
				if (string == NULL)
					perror("malloc");
				strcpy(string, argv[index]);
				playlist_add_song(index - 1, string, &cur_playlist);
				num_songs = argc - 2;
			}
		}
		// for mount
		/*
		else if (strcmp(argv[1], "-usb") == 0)
		{
			mountFlag = mountToggle(1, argv[2]);
			if (mountFlag == MOUNTED)
			{
				//printf("arg2: %s\n", argv[2]);
				cur_playlist = reReadPlaylist(argv[2]);
				if (num_songs == 0)
				{
					printf("No songs found in mount %s\n", argv[2]);
					return -1;
				}
			}
			else if (mountFlag == MOUNT_ERROR)
			{
				printf("Mount Error - Can not mount %s\n", argv[2]);
				return -2;
			}
		}
		*/
		else if (strcmp(argv[1], "-dir") == 0)
		{
			cur_playlist = reReadPlaylist(argv[2]);
			if (num_songs == 0)
			{
				printf("No songs found in directory %s\n", argv[2]);
				return -1;
			}
		}
		else
			return usage(argv[0]);
	}
	else
		return usage(argv[0]);
  	if (wiringPiSetup () == -1)
  	{
	    fprintf(stdout, "oops: %s\n", strerror(errno));
	    return 1;
	}
	for (i = 0; i < 5; i++)
	{
		pinMode(buttonPins[i], INPUT);
		pullUpDnControl(buttonPins[i], PUD_UP);
	}
	/*
	int t_ret;
	t_ret = piThreadCreate(songThread);
	*/
	// First test at button pausing/playing
	//wiringPiISR(stopButtonPin, INT_EDGE_RISING, &stopSong);
	lcdHandle = lcdInit(RO, CO, BS, RS, EN, D0, D1, D2, D3, D0, D1, D2, D3);
	if (lcdHandle < 0)
	{
		fprintf(stderr, "%s: lcdInit failed\n", argv[0]);
		return -1;
	}
	// ncurses stuff (to be removed in final product)
	if (LCD_ONLY == FALSE)
	{
		initscr();
		noecho();
		cbreak();
		nodelay(stdscr, true);
		curs_set(0);
		printw("--= Pi LCD-MP3 Player =--");
		mvaddstr(2, 0, "by John Wiggins");
		mvaddstr(3, 0, "Controls:");
		//mvaddstr(4, 0, "           m - (un)mount USB toggle");
		mvaddstr(5, 0, "           n - next song");
		mvaddstr(6, 0, "           p - previous song");
		mvaddstr(7, 0, "       space - pause/play song");
		mvaddstr(8, 0, "           i - toggle info");
		mvaddstr(9, 0, "           q - quit program");
		refresh();
	}
	song_index = 1;
	cur_song.play_status = PLAY;
	while (cur_song.play_status != QUIT && song_index < num_songs)
	{
		if (LCD_ONLY == FALSE)
		{
			// Clear the lines for song title, artist, album - ncurses
			int clear_i;
			for (clear_i = 10; clear_i < 16; clear_i++)
				mvaddstr(clear_i, 0,   "                                                                     ");
		}
		string = malloc(MAXDATALEN);
		if (string == NULL)
			perror("malloc");
		playlist_get_song(song_index, &string, &cur_playlist);
		if (string != NULL)
		{
			basec = strdup(string);
			bname = basename(basec);
			strcpy(cur_song.filename, string);
			strcpy(cur_song.base_filename, bname);
			// See if we can get the song info from the file.
			id3_tagger();
			if (LCD_ONLY == FALSE)
			{
				//move(1, 0);
				//printw((mountFlag == MOUNTED ? "  Mounted" : "Unmounted"));
				move(10, 0);
				printw("------------------");
				move(11, 0);
				printw("Song Number: %d / %d", song_index, num_songs);
				move(12, 0);
				printw(" Song Title: %s", cur_song.title);
				move(13, 0);
				printw("     Artist: %s", cur_song.artist);
				move(14, 0);
				printw("      Album: %s", cur_song.album);
				move(15, 0);
				printw("      Genre: %s", cur_song.genre);
				refresh();
			}
			// play the song as a thread
			pthread_create(&song_thread, NULL, (void *) play_song, (void *) &cur_song);
			// Should we detach the thread? ... TODO
			//pthread_detach(song_thread);
			// The following displays stuff to the LCD without scrolling
			scroll_firstRow_flag = printLcdFirstRow();
			scroll_secondRow_flag = printLcdSecondRow();
			// loop to play the song
			while (cur_song.song_over == FALSE)
			{
				// Following code is to scroll the song info
				if (scroll_firstRow_flag == TRUE)
					scrollMessage_firstRow();
				if (scroll_secondRow_flag == TRUE)
					scrollMessage_secondRow();
				if (useButtonFlag == TRUE)
				{
					// Play / Pause
					btnCtr = 0;
					buttonState = digitalRead(playButtonPin);
					if (buttonState == LOW)
					{
						// check 3 times to make sure the button was actually pressed
						for (i = 0; i < 3; i++)
						{
							buttonState = digitalRead(playButtonPin);
							if (buttonState == LOW)
								btnCtr++;
							delay(BTN_DELAY);
						}
					}
					if (btnCtr >= 3)
					{
						if (cur_song.play_status == PAUSE)
						{
							playMe();
							pthread_mutex_lock(&cur_song.pauseMutex);
							strcpy(cur_song.second_row_text, pause_text);
							lcdPosition(lcdHandle, 0, 1);
							lcdPuts(lcdHandle, lcd_clear);
							scroll_secondRow_flag = printLcdSecondRow();
							pthread_mutex_unlock(&cur_song.pauseMutex);
						}
						else
						{
							pauseMe();
							// copy whatever is currently on the second row
							pthread_mutex_lock(&cur_song.pauseMutex);
							strcpy(pause_text, cur_song.second_row_text);
							strcpy(cur_song.second_row_text, "PAUSED");
							lcdPosition(lcdHandle, 0, 1);
							lcdPuts(lcdHandle, lcd_clear);
							scroll_secondRow_flag = printLcdSecondRow();
							pthread_mutex_unlock(&cur_song.pauseMutex);
						}
					}
					// don't even check to see if the prev/next/info/quit buttons
					// have been pressed if we are in a pause state.
					if (cur_song.play_status != PAUSE)
					{
						btnCtr = 0;
						buttonState = digitalRead(prevButtonPin);
						if (buttonState == LOW)
						{
							for (i = 0; i < 3; i++)
							{
								buttonState = digitalRead(prevButtonPin);
								if (buttonState == LOW)
									btnCtr++;
								delay(BTN_DELAY);
							}
						}
						if (btnCtr >= 3)
						{
							if (song_index - 1 != 0)
							{
								prevSong();
								song_index--;
							}
						}
						btnCtr = 0;
						buttonState = digitalRead(nextButtonPin);
						if (buttonState == LOW)
						{
							for (i = 0; i < 3; i++)
							{
								buttonState = digitalRead(nextButtonPin);
								if (buttonState == LOW)
									btnCtr++;
								delay(BTN_DELAY);
							}
						}
						if (btnCtr >= 3)
						{
							if (song_index + 1 < num_songs)
							{
								nextSong();
								song_index++;
							}
						}
						btnCtr = 0;
						buttonState = digitalRead(infoButtonPin);
						if (buttonState == LOW)
						{
							for (i = 0; i < 3; i++)
							{
								buttonState = digitalRead(infoButtonPin);
								if (buttonState == LOW)
									btnCtr++;
								delay(BTN_DELAY);
							}
						}
						if (btnCtr >= 3)
						{
							// toggle what to display
							pthread_mutex_lock(&cur_song.pauseMutex);
							strcpy(cur_song.second_row_text, (strcmp(cur_song.second_row_text, cur_song.artist) == 0 ? cur_song.album : cur_song.artist));
							// first clear just the second row, then re-display the second row
							lcdPosition(lcdHandle, 0, 1);
							lcdPuts(lcdHandle, lcd_clear);
							scroll_secondRow_flag = printLcdSecondRow();
							pthread_mutex_unlock(&cur_song.pauseMutex);
						}
						btnCtr = 0;
						buttonState = digitalRead(quitButtonPin);
						if (buttonState == LOW)
						{
							for (i = 0; i < 3; i++)
							{
								buttonState = digitalRead(quitButtonPin);
								if (buttonState == LOW)
									btnCtr++;
								delay(BTN_DELAY);
							}
						}
						if (btnCtr >= 3)
						{
							quitMe();
						}
					}
					/*
					else
					{
						if (LCD_ONLY == FALSE)
						{
							move(18, 0);
							printw("  paused");
						}
					}
					*/
				}
				if (LCD_ONLY == FALSE)
				{
					key = getch();
					if (key > -1)
					{
						if (key == ' ')
						{
							if (cur_song.play_status == PAUSE)
							{
								playMe();
								pthread_mutex_lock(&cur_song.pauseMutex);
								strcpy(cur_song.second_row_text, pause_text);
								lcdPosition(lcdHandle, 0, 1);
								lcdPuts(lcdHandle, lcd_clear);
								scroll_secondRow_flag = printLcdSecondRow();
								pthread_mutex_unlock(&cur_song.pauseMutex);
								move(18, 0);
								printw("      ");
							}
							else
							{
								pauseMe();
								// copy whatever is currently on the second row
								pthread_mutex_lock(&cur_song.pauseMutex);
								strcpy(pause_text, cur_song.second_row_text);
								strcpy(cur_song.second_row_text, "PAUSED");
								lcdPosition(lcdHandle, 0, 1);
								lcdPuts(lcdHandle, lcd_clear);
								scroll_secondRow_flag = printLcdSecondRow();
								pthread_mutex_unlock(&cur_song.pauseMutex);
								move(18, 0);
								printw("paused");
							}
						}
						// don't even check to see if the prev/next/info/quit buttons
						// have been pressed if we are in a pause state.
						if (cur_song.play_status != PAUSE)
						{
							if (key == 'n')
							{
								// don't go to next song if last song
								// TODO maybe just quit the program if it is the last song...
								if (song_index + 1 < num_songs)
								{
									nextSong();
									song_index++;
								}
							}
							else if (key == 'p')
							{
								// don't go back if at first song
								if (song_index - 1 != 0)
								{
									prevSong();
									song_index--;
								}
							}
							else if (key == 'q')
							{
								quitMe();
							}
							else if (key == 'i')
							{
								// toggle what to display
								pthread_mutex_lock(&cur_song.pauseMutex);
								strcpy(cur_song.second_row_text, (strcmp(cur_song.second_row_text, cur_song.artist) == 0 ? cur_song.album : cur_song.artist));
								// first clear just the second row, then re-display the second row
								lcdPosition(lcdHandle, 0, 1);
								lcdPuts(lcdHandle, lcd_clear);
								scroll_secondRow_flag = printLcdSecondRow();
								pthread_mutex_unlock(&cur_song.pauseMutex);
							}
							/*
							else if (key == 'm')
							{
								mountFlag = mountToggle(2, "/MUSIC");
								//move(20, 0); printw("mountflag: %d", mountFlag);
								if (mountFlag == MOUNTED)
								{
									//pthread_mutex_lock(&cur_song.pauseMutex);
									tmp_playlist = reReadPlaylist("/MUSIC");
									if (num_songs != 0)
									{
										cur_playlist = tmp_playlist;
										cur_song.song_over = TRUE;
										song_index = 1;
									}
									//move(21, 0); printw("after readplaylist");
									//pthread_mutex_unlock(&cur_song.pauseMutex);
								}
								// FIXME maybe pause everything until it is mounted?
								else if (mountFlag == UNMOUNTED)
								{
									//pthread_mutex_lock(&cur_song.pauseMutex);
									cur_playlist = reReadPlaylist("/root/Music");
									cur_song.song_over = TRUE;
									song_index = 1;
									//pthread_mutex_unlock(&cur_song.pauseMutex);
								}
								else // FIXME
								{
									//pthread_mutex_lock(&cur_song.pauseMutex);
									cur_playlist = reReadPlaylist("/root/Music");
									cur_song.song_over = TRUE;
									song_index = 1;
									//pthread_mutex_unlock(&cur_song.pauseMutex);
								}
							}
							*/
						}
					}
				}
			}
			if (pthread_join(song_thread, NULL) != 0)
				perror("join error\n");
			// clear the lcd for next song.
			lcdClear(lcdHandle);
		}
		lcdClear(lcdHandle);
		// reset all the flags.
		scroll_firstRow_flag = scroll_secondRow_flag = FALSE;
		// increment the song_index if the song is over but the next/prev wasn't hit
		if (cur_song.song_over == TRUE && cur_song.play_status == PLAY)
		{
			pthread_mutex_lock(&cur_song.pauseMutex);
			cur_song.song_over = FALSE;
			pthread_mutex_unlock(&cur_song.pauseMutex);
			song_index++;
		} // FIXME change mountFlag to cur_song.play_status == NEW
		//else if (cur_song.song_over == TRUE && (mountFlag == MOUNTED || (cur_song.play_status == NEXT || cur_song.play_status == PREV)))
		else if (cur_song.song_over == TRUE && (cur_song.play_status == NEXT || cur_song.play_status == PREV))
		{
			pthread_mutex_lock(&cur_song.pauseMutex);
			// empty out song/artist data
			strcpy(cur_song.title, "");
			strcpy(cur_song.artist, "");
			strcpy(cur_song.album, "");
			cur_song.play_status = PLAY;
			cur_song.song_over = FALSE;
			pthread_mutex_unlock(&cur_song.pauseMutex);
		}
	}
	lcdClear(lcdHandle);
	lcdPosition(lcdHandle, 0, 0);
	lcdPuts(lcdHandle, "Good Bye!");
	delay(1000);
	lcdClear(lcdHandle);
	if (LCD_ONLY == FALSE)
		endwin();
	return 0;
}
