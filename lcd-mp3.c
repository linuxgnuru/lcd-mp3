/*
 *	lcd-mp3
 *
 *	John Wiggins (jcwiggi@gmail.com)
 *
 *	mp3 player for Raspberry Pi with output to a 16x2 LCD display for song information
 *
 *	Portions of this code were borrowed from MANY other projects including (but not limited to)
 *
 *	- wiringPi example code for lcd.c http://wiringpi.com/
 *		- Many thanks to Gordon for making the wiringPi library.
 *
 *	- http://hzqtc.github.io/2012/05/play-mp3-with-libmpg123-and-libao.html
 *
 *	- http://www.arduino.cc/en/Tutorial/Debounce
 *
 *	- Many thanks to those who helped me out at StackExchange
 *	  (http://raspberrypi.stackexchange.com/)
 *
 *  Known issues:
 *
 *	- The MP3 decoding part I use for some reason always gives me an error on STDERR and I haven't the
 *	  time to go through the lib sources to try to find out what's going on; so I always just run the
 *	  program with 2>/dev/null (e.g. lcd-mp3-usb 2>/dev/null)
 *
 * Unknown issues:
 *
 *	- see recusion
 *
 * -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 *	Requires:
 *	- Program must be run as root.
 *	- Libraries:
 *		pthread
 *		wiringPi
 *		ao
 *		mpg123
 *	- System setup:
 *		- A directory /MUSIC needs to be created.
 *		- In the file, /etc/fstab the following entry needs to be added:
 *
 *		/dev/sda1       /MUSIC          vfat    defaults          0       2
 *
 * -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 *	CHANGELOG
 *	---------
 *	04-04-2015	Trying new debounce.
 *	04-04-2015	Try to fix LCD bug
 *	03-04-2015	TODO find better way for debouncing buttons; maybe have to do in hardware.
 *	03-04-2015	Changed shutdown/halt option (now you have to add -halt) Also tried to fix scrolling text
 *			delay.  Song file names with spaces seem to work; wonder what else is wrong.
 *			- FIXME - found odd bug; sometimes when paused, and is then unpaused the text gets glitchy.
 *	31-01-2015	Setting up for final project.
 *			Things to be done:
 *				- Remove all ncurses / terminal output
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

// Nice way to include debuging prints without having to keep commenting out lines
// just have to comment out the following line:
//#define DEBUG 1

// --------- END USER MODIFIABLE VARS ---------


/*
 * Debounce tracking stuff
 */
int  playButtonState,      prevButtonState,      nextButtonState,      infoButtonState,      quitButtonState;
int  lastPlayButtonState,  lastPrevButtonState,  lastNextButtonState,  lastInfoButtonState,  lastQuitButtonState;
long lastPlayDebounceTime, lastPrevDebounceTime, lastNextDebounceTime, lastInfoDebounceTime, lastQuitDebounceTime;
long debounceDelay = 50;

const int buttonPins[] = {
	playButtonPin,
	prevButtonPin,
	nextButtonPin,
	infoButtonPin,
	quitButtonPin
	};

int usage(const char *progName)
{
	fprintf(stderr, "Usage: %s [OPTION] \n"
		"-pins (shows what pins to use for buttons) \n"
		"-dir [dir] \n"
		"-songs [MP3 files]\n"
		"-usb (this reads in any music found in /MUSIC)\n"
		"-halt (this is only if -usb is also used;\n"
		"       allows the program to halt the system after\n"
		"       the 'quit' button was pressed.)\n",
		progName);
	return EXIT_FAILURE;
}

/*
 * linked list / playlist functions
 */

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

/*
 * Mounting function; might use it in the future
 */

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

// Check to see if the USB flash was mounted or not.
// mount (if cmd == 1, do not attempt to unmount)
int checkMount()
{
	if (mount("/dev/sda1", "/MUSIC", "vfat", MS_RDONLY | MS_SILENT, "") == -1)
	{
		if (errno == EBUSY)
			return MOUNTED;
		else
			return MOUNT_ERROR;
	}
	else
		return MOUNTED;
}

/*
 * Creates playlist
 */

// if USB has been mounted, load in songs.
playlist_t reReadPlaylist(char *dir_name)
{
	int index = 0;
	char *string;
	playlist_t new_playlist;
	DIR *d;
	struct dirent *dir;

	playlist_init(&new_playlist);
	d = opendir(dir_name);
	if (d)
	{
		while ((dir = readdir(d)) != NULL)
		{
			if (dir->d_type == 8)
			{
				index = (index == 0 ? 1 : index);
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

/*
 * Threading functions
 */
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

/*
 * MP3 ID3 tag - Attempt to get song/artist/album names from file
 */

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
		sprintf(cur_song.title,  "UNKNOWN");
		sprintf(cur_song.artist, "UNKNOWN");
		sprintf(cur_song.album,  "UNKNOWN");
		sprintf(cur_song.genre,  "UNKNOWN");
	}
	// if there is no title to be found, set title to the song file name.
	if (strlen(cur_song.title) == 0)
		strcpy(cur_song.title, cur_song.base_filename);
	if (strlen(cur_song.artist) == 0)
		sprintf(cur_song.artist, "UNKNOWN");
	if (strlen(cur_song.album) == 0)
		sprintf(cur_song.album, "UNKNOWN");
	// set the second row to be the artist by default.
	strcpy(cur_song.FirstRow_text, cur_song.title);
	strcpy(cur_song.SecondRow_text, cur_song.artist);
	mpg123_close(m);
	mpg123_delete(m);
	mpg123_exit();
	// the following two lines are just to see when the scrolling should pause
	strncpy(cur_song.scroll_FirstRow, cur_song.FirstRow_text, 15);
	strncpy(cur_song.scroll_SecondRow, cur_song.SecondRow_text, 16);
	return 0;
}

/*
 * LCD display functions
 */

// non-scrolling
int printLcdFirstRow()
{
	int flag = TRUE;
	
	if (strcmp(cur_song.FirstRow_text, " QUIT - Shutdown") == 0)
	{
		lcdPosition(lcdHandle, 0, 0);
		lcdPuts(lcdHandle, cur_song.FirstRow_text);
		flag = FALSE;
	}
	else
	{
		// have to set to 15 because of music note
		if (strlen(cur_song.FirstRow_text) < 15)
		{
			// new song; set the previous title
			if (strcmp(cur_song.title, cur_song.prevTitle) != 0)
				strcpy(cur_song.prevTitle, cur_song.title);
			lcdCharDef(lcdHandle, 2, musicNote);
			lcdPosition(lcdHandle, 0, 0);
			lcdPutchar(lcdHandle, 2);
			lcdPosition(lcdHandle, 1, 0);
			lcdPuts(lcdHandle, cur_song.FirstRow_text);
			flag = FALSE;
		}
	}
	return flag;
}

// non-scrolling
int printLcdSecondRow()
{
	int flag = TRUE;

	if (strlen(cur_song.SecondRow_text) < 16)
	{
		lcdPosition(lcdHandle, 0, 1);
		lcdPuts(lcdHandle, cur_song.SecondRow_text);
		flag = FALSE;
		// new song; set the previous artist
		if (strcmp(cur_song.artist, cur_song.prevArtist) != 0)
			strcpy(cur_song.prevArtist, cur_song.artist);
	}
	return flag;
}

void scrollMessage_FirstRow(int *pauseScroll_FirstRow_Flag)
{
	char buf[32];
	char my_songname[MAXDATALEN];
	static int position = 0;
	static int timer = 0;
	int width = 15;

	if (strcmp(cur_song.title, cur_song.prevTitle) != 0)
	{
		timer = 0;
		position = 0;
		strcpy(cur_song.prevTitle, cur_song.title);
	}
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
	if (position == (strlen(my_songname) - width))
		position = 0;
	// pause briefly when text reaches begining line before continuing
	*pauseScroll_FirstRow_Flag = (strcmp(buf, cur_song.scroll_FirstRow) == 0 ? TRUE : FALSE);
}

void scrollMessage_SecondRow(int *pauseScroll_SecondRow_Flag)
{
	char buf[32];
	static int position = 0;
	static int timer = 0;
	int width = 16;
	char my_string[MAXDATALEN];

	if (strcmp(cur_song.artist, cur_song.prevArtist) != 0)
	{
		timer = 0;
		position = 0;
		strcpy(cur_song.prevArtist, cur_song.artist);
	}
	strcpy(my_string, spaces);
	strncat(my_string, cur_song.SecondRow_text, strlen(cur_song.SecondRow_text));
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
	if (position == (strlen(my_string) - width))
		position = 0;
	// pause briefly when text reaches begining line before continuing
	*pauseScroll_SecondRow_Flag = (strcmp(buf, cur_song.scroll_SecondRow) == 0 ? TRUE : FALSE);
}

// The actual thing that plays the song
void play_song(void *arguments)
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
		ao_play(dev, (char *) buffer, done);
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
	{
		args->play_status = PLAY;
	}
	cur_status.song_over = TRUE;
	pthread_mutex_unlock(&(cur_song.writeMutex));
}

int main(int argc, char **argv)
{
	pthread_t song_thread;
	playlist_t cur_playlist;
	clock_t startPauseFirstRow;  // for pausing scroll display
	clock_t startPauseSecondRow; // for pausing scroll display
	char *basec, *bname;
	char *string;
	char pause_text[MAXDATALEN];
	char lcd_clear[] = "                ";
	int index;
	int song_index;
	int i;
	int ctrSecondRowScroll;
	int reading;
	// Flags
	int mountFlag;
	int haltFlag = FALSE;
	int scroll_FirstRow_flag;
	int scroll_SecondRow_flag;
	int pauseScroll_FirstRow_Flag = FALSE;
	int pauseScroll_SecondRow_Flag = FALSE;
	int firstTime_FirstRow_Flag = TRUE;
	int firstTime_SecondRow_Flag = TRUE;
	int temp_FirstRow_Flag = FALSE;
	int temp_SecondRow_Flag = FALSE;

	// Initializations
	playlist_init(&cur_playlist);
	ctrSecondRowScroll = 0;
	cur_song.song_over = FALSE;
	scroll_FirstRow_flag = scroll_SecondRow_flag = FALSE;
	lastPlayButtonState = lastPrevButtonState = lastNextButtonState = lastInfoButtonState = lastQuitButtonState = HIGH;
	lastPlayDebounceTime = lastPrevDebounceTime = lastNextDebounceTime = lastInfoDebounceTime = lastQuitDebounceTime = 0;
	startPauseFirstRow = clock();
	startPauseSecondRow = clock();
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
			       "Quit    \t7       \t4\n\n");
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
			// FIXME I'm lazy right now; just threw this in so the test at the end
			// won't fail.
			mountFlag = MOUNTED;
		}
		/***************************************************
		 *  This is the USB auto-startup mode              *
		 *                                                 *
		 *  i.e. in /etc/rc.local add:                     *
		 *                                                 *
		 *  /usr/local/bin/lcd-mp3 -usb 2>/dev/null        *
		 *                                                 *
		 *  if you wish the system to be able to be        *
		 *  shutdown after it quits:                       *
		 *                                                 *
		 *  /usr/local/bin/lcd-mp3 -usb -halt 2>/dev/null  *
		 ***************************************************/
		else if (strcmp(argv[1], "-usb") == 0)
		{
			// First, check if USB is mounted.
			mountFlag = checkMount();
			if (mountFlag == MOUNTED)
			{
				cur_playlist = reReadPlaylist("/MUSIC");
			}
			if (argc == 3)
			{
				if (strcmp(argv[2], "-halt") == 0)
				{
					haltFlag = TRUE;
				}
			}
		}
		else if (strcmp(argv[1], "-dir") == 0)
		{
			cur_playlist = reReadPlaylist(argv[2]);
			if (num_songs == 0)
			{
				printf("No songs found in directory %s\n", argv[2]);
				return -1;
			}
			// FIXME I'm lazy right now; just threw this in so the test at the end
			// won't fail.
			mountFlag = MOUNTED;
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
	// First test at button pausing/playing
	lcdHandle = lcdInit(RO, CO, BS, RS, EN, D0, D1, D2, D3, D0, D1, D2, D3);
	if (lcdHandle < 0)
	{
		fprintf(stderr, "%s: lcdInit failed\n", argv[0]);
		return -1;
	}
	song_index = 1;
	cur_song.play_status = PLAY;
	strcpy(cur_song.prevTitle, cur_song.title);
	strcpy(cur_song.prevArtist, cur_song.artist);
	while (cur_song.play_status != QUIT && song_index < num_songs)
	{
		string = malloc(MAXDATALEN);
		if (string == NULL)
			perror("malloc");
		playlist_get_song(song_index, (void **) &string, &cur_playlist);
		if (string != NULL)
		{
			basec = strdup(string);
			bname = basename(basec);
			strcpy(cur_song.filename, string);
			strcpy(cur_song.base_filename, bname);
			// See if we can get the song info from the file.
			id3_tagger();
			// play the song as a thread
			pthread_create(&song_thread, NULL, (void *) play_song, (void *) &cur_song);
			// The following displays stuff to the LCD without scrolling
			scroll_FirstRow_flag = printLcdFirstRow();
			scroll_SecondRow_flag = printLcdSecondRow();
			// loop to play the song
			while (cur_song.song_over == FALSE)
			{
				// First row song-name
				if (cur_song.play_status != PAUSE)
				{
					if (scroll_FirstRow_flag == TRUE)
					{
						/* 
						 * NOTE; the following use of tons of flags and such are
						 * used to avoid having to use the delay function which
						 * was causing some problems.
						 */
						if (firstTime_FirstRow_Flag == TRUE)
						{
							firstTime_FirstRow_Flag = FALSE;
							scrollMessage_FirstRow(&pauseScroll_FirstRow_Flag);
						}
						else
						{
							// start the timer
							if (pauseScroll_FirstRow_Flag == TRUE && temp_FirstRow_Flag == FALSE)
							{
								startPauseFirstRow = clock();
								temp_FirstRow_Flag = TRUE;
							}
							if (temp_FirstRow_Flag == TRUE)
							{
								// check to see if 1 second has passed
								if ((int)((double)(clock() - startPauseFirstRow) / CLOCKS_PER_SEC) == 1)
								{
									pauseScroll_FirstRow_Flag = FALSE;
									temp_FirstRow_Flag = FALSE;
								}
							}
						}
						if (pauseScroll_FirstRow_Flag == FALSE)
							scrollMessage_FirstRow(&pauseScroll_FirstRow_Flag);
					}
					// Second row (artist / album)
					if (scroll_SecondRow_flag == TRUE)
					{
						if (firstTime_SecondRow_Flag == TRUE)
						{
							firstTime_SecondRow_Flag = FALSE;
							scrollMessage_SecondRow(&pauseScroll_SecondRow_Flag);
							ctrSecondRowScroll = 1;
						}
						else
						{
							if (pauseScroll_SecondRow_Flag == TRUE && temp_SecondRow_Flag == FALSE)
							{
								startPauseSecondRow = clock();
								temp_SecondRow_Flag = TRUE;
							}
							if (temp_SecondRow_Flag == TRUE)
							{
								if ((int)((double)(clock() - startPauseSecondRow) / CLOCKS_PER_SEC) == 1)
								{
									pauseScroll_SecondRow_Flag = FALSE;
									temp_SecondRow_Flag = FALSE;
									ctrSecondRowScroll++;
								}
							}
						}
						// Only scroll 2 times; after that just always display.
						if (ctrSecondRowScroll > 2)
							pauseScroll_SecondRow_Flag = TRUE;
						if (pauseScroll_SecondRow_Flag == FALSE)
							scrollMessage_SecondRow(&pauseScroll_SecondRow_Flag);
					}
				}
				/*
				 * Play / Pause button
				 */
				 /*
				  * NOTE:
				  * I got the following debouncing code from
				  * http://www.arduino.cc/en/Tutorial/Debounce 
				  *
				  * Excerpt from source:
				  *
 				  *  created 21 Nov 2006 by David A. Mellis
  				  * modified 30 Aug 2011 by Limor Fried
  				  * modified 28 Dec 2012 by Mike Walters
  				  * This example code is in the public domain.
				  */
				reading = digitalRead(playButtonPin);
				// check to see if you just pressed the button 
				// (i.e. the input went from HIGH to LOW),  and you've waited 
				// long enough since the last press to ignore any noise:  
				// If the switch changed, due to noise or pressing:
				if (reading != lastPlayButtonState)
					lastPlayDebounceTime = millis(); // reset the debouncing timer
				if ((millis() - lastPlayDebounceTime) > debounceDelay)
				{
					// whatever the reading is at, it's been there for longer
					// than the debounce delay, so take it as the actual current state:
					// if the button state has changed:
					if (reading != playButtonState)
					{
						playButtonState = reading;
						if (playButtonState == LOW)
						{
							if (cur_song.play_status == PAUSE)
							{
								playMe();
								strcpy(cur_song.SecondRow_text, pause_text);
								lcdPosition(lcdHandle, 0, 1);
								lcdPuts(lcdHandle, lcd_clear);
								scroll_SecondRow_flag = printLcdSecondRow();
							}
							else
							{
								pauseMe();
								// copy whatever is currently on the second row
								strcpy(pause_text, cur_song.SecondRow_text);
								strcpy(cur_song.SecondRow_text, "PAUSED");
								strcpy(cur_song.prevArtist, cur_song.artist);
								lcdPosition(lcdHandle, 0, 1);
								lcdPuts(lcdHandle, lcd_clear);
								scroll_SecondRow_flag = printLcdSecondRow();
							}
						}
					}
				}
				// save the reading. Next time through the loop, it'll be the lastButtonState:
				lastPlayButtonState = reading;
				// don't even check to see if the prev/next/info/quit buttons
				// have been pressed if we are in a pause state.
				if (cur_song.play_status != PAUSE)
				{
					/*
					 * Previous button
					 */
					reading = digitalRead(prevButtonPin);
					if (reading != lastPrevButtonState)
						lastPrevDebounceTime = millis();
					if ((millis() - lastPrevDebounceTime) > debounceDelay)
					{
						if (reading != prevButtonState)
						{
							prevButtonState = reading;
							if (prevButtonState == LOW)
							{
								if (song_index - 1 != 0)
								{
									prevSong();
									song_index--;
								}
							}
						}
					}
					lastPrevButtonState = reading;
					/*
					 * Next button
					 */
					reading = digitalRead(nextButtonPin);
					if (reading != lastNextButtonState)
						lastNextDebounceTime = millis();
					if ((millis() - lastNextDebounceTime) > debounceDelay)
					{
						if (reading != nextButtonState)
						{
							nextButtonState = reading;
							if (nextButtonState == LOW)
							{
								if (song_index + 1 < num_songs)
								{
									nextSong();
									song_index++;
								}
							}
						}
					}
					lastNextButtonState = reading;
					/*
					 * Info button
					 */
					reading = digitalRead(infoButtonPin);
					if (reading != lastInfoButtonState)
						lastInfoDebounceTime = millis();
					if ((millis() - lastInfoDebounceTime) > debounceDelay)
					{
						if (reading != infoButtonState)
						{
							infoButtonState = reading;
							if (infoButtonState == LOW)
							{
								// toggle what to display
								strcpy(cur_song.SecondRow_text, (strcmp(cur_song.SecondRow_text, cur_song.artist) == 0 ? cur_song.album : cur_song.artist));
								// first clear just the second row, then re-display the second row
								lcdPosition(lcdHandle, 0, 1);
								lcdPuts(lcdHandle, lcd_clear);
								scroll_SecondRow_flag = printLcdSecondRow();
							}
						}
					}
					lastInfoButtonState = reading;
					/*
					 * Quit button
					 */
					reading = digitalRead(quitButtonPin);
					if (reading != lastQuitButtonState) lastQuitDebounceTime = millis();
					if ((millis() - lastQuitDebounceTime) > debounceDelay)
					{
						if (reading != quitButtonState)
						{
							quitButtonState = reading;
							if (quitButtonState == LOW)
								quitMe();
						}
					}
					lastQuitButtonState = reading;
				}
			}
			// reset all the flags.
			scroll_FirstRow_flag = scroll_SecondRow_flag = FALSE;
			pauseScroll_FirstRow_Flag = pauseScroll_SecondRow_Flag = FALSE;
			firstTime_FirstRow_Flag = firstTime_SecondRow_Flag = TRUE;
			temp_FirstRow_Flag = temp_SecondRow_Flag = FALSE;
			ctrSecondRowScroll = 0;
			if (pthread_join(song_thread, NULL) != 0)
				perror("join error\n");
			// clear the lcd for next song.
			lcdClear(lcdHandle);
		}
		lcdClear(lcdHandle);
		// increment the song_index if the song is over but the next/prev wasn't hit
		if (cur_song.song_over == TRUE && cur_song.play_status == PLAY)
		{
			pthread_mutex_lock(&cur_song.pauseMutex);
			cur_song.song_over = FALSE;
			pthread_mutex_unlock(&cur_song.pauseMutex);
			song_index++;
		}
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
	if (mountFlag == MOUNT_ERROR)
	{
		lcdClear(lcdHandle);
		lcdPosition(lcdHandle, 0, 0);
		lcdPuts(lcdHandle, "No USB inserted.");
		lcdPosition(lcdHandle, 0, 1);
		if (haltFlag == TRUE)
		{
			lcdPuts(lcdHandle, "Shutting down.");
			delay(1000);
			system("shutdown -h now");
		}
		else
			lcdPuts(lcdHandle, "Please shutdown.");
	}
	else
	{
		if (num_songs == 0)
		{
			lcdClear(lcdHandle);
			lcdPosition(lcdHandle, 0, 0);
			lcdPuts(lcdHandle, "No songs on USB.");
			lcdPosition(lcdHandle, 0, 1);
			if (haltFlag == TRUE)
			{
				lcdPuts(lcdHandle, "Shutting down.");
				delay(1000);
				system("shutdown -h now");
			}
			else
				lcdPuts(lcdHandle, "Please shutdown.");
		}
		else
		{
			lcdClear(lcdHandle);
			delay(1000);
			lcdClear(lcdHandle);
			// Don't shutdown unless the quit button was pressed.
			if (cur_song.play_status == QUIT)
			{
				lcdPosition(lcdHandle, 0, 0);
				lcdPuts(lcdHandle, "Good Bye!");
				lcdPosition(lcdHandle, 0, 1);
				if (haltFlag == TRUE)
				{
					lcdPuts(lcdHandle, "Shuting down.");
					delay(1000);
					system("shutdown -h now");
				}
				else
					lcdPuts(lcdHandle, "Please shutdown.");
			}
			else
			{
				// TODO Maybe instead of getting here, we might just repeat the song list somehow?
				lcdPosition(lcdHandle, 0, 0);
				lcdPuts(lcdHandle, "No more songs.");
				lcdPosition(lcdHandle, 0, 1);
				if (haltFlag == TRUE)
				{
					lcdPuts(lcdHandle, "Shuting down.");
					delay(1000);
					system("shutdown -h now");
				}
				else
					lcdPuts(lcdHandle, "Please shutdown.");
			}
		}
	}
	return 0;
}
