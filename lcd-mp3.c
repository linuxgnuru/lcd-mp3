/*
 *  lcd-mp3
 *
 *  John Wiggins (jcwiggi@gmail.com)
 *
 *  mp3 player for Raspberry Pi with output to a 16x2 LCD display for song information
 *
 *  Portions of this code were borrowed from MANY other projects including (but not limited to)
 *
 *  - wiringPi example code for lcd.c http://wiringpi.com/
 *    - Many thanks to Gordon for making the wiringPi library.
 *
 *  - http://hzqtc.github.io/2012/05/play-mp3-with-libmpg123-and-libao.html
 *
 *  - http://www.arduino.cc/en/Tutorial/Debounce
 *
 *  - Many thanks to those who helped me out at StackExchange
 *    (http://raspberrypi.stackexchange.com/)
 *
 *  Known issues:
 *
 *  - The MP3 decoding part I use for some reason always gives me an error on STDERR and I haven't the
 *    time to go through the lib sources to try to find out what's going on; so I always just run the
 *    program with 2>/dev/null (e.g. lcd-mp3-usb 2>/dev/null)
 *
 * -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 *  Requires:
 *  - Program must be run as root or type "sudo chmod +s [executable]"
 *  - Libraries:
 *    pthread
 *    ao
 *    mpg123
 *    (all the above are available via apt-get if using raspbian)
 *
 *    wiringPi (available via: git clone git://git.drogon.net/wiringPi )
 *  - System setup:
 *    - A directory /MUSIC needs to be created.
 *    (optional as the program will attempt to mount the flash)
 *    - In the file, /etc/fstab, the following entry needs to be added so the usb flash will be mounted:
 *
 *    /dev/sda1       /MUSIC          vfat    defaults          0       2
 *
 * -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 * NOTE: Changelog is now in its own file ChangeLog
 *
 * -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <libgen.h>
#include <signal.h>

// For mounting
#include <sys/mount.h>
#include <dirent.h> 

// For wiringPi
#include <wiringPi.h>
#include <lcd.h>

#include "lcd-mp3.h"

// --------- BEGIN USER MODIFIABLE VARS ---------

// GPIO pins (using wiringPi numbers)
#define playButtonPin   0 // GPIO 0, BCM 17
#define prevButtonPin   1 // GPIO 1, BCM 18
#define nextButtonPin   2 // GPIO 2, BCM 27
#define infoButtonPin   5 // GPIO 5, BCM 24
#define quitButtonPin   7 // GPIO 7, BCM  4
#define shufButtonPin  11 // CE1,    BCM  7

// The following is used to test to see if the LCD/button board is attached or not.
#define boardTestPin   10 // CE0,    BCM  8

// Pins for the LCD display
const int BS = 4;	// Bits (4 or 8)
const int CO = 16;	// Number of columns
const int RO = 2;	// Number of rows

const int RS = 3;	// GPIO 3
const int EN = 14;	// SCK (SPI)
const int D0 = 4;	// GPIO 4
const int D1 = 12;	// MOSI (SPI)
const int D2 = 13;	// MISO (SPI)
const int D3 = 6;	// GPIO 6

#define BTN_DELAY 30

//#define DEBUG 0

// --------- END USER MODIFIABLE VARS ---------

/*
 * Debounce tracking stuff
 */
int  playButtonState, prevButtonState, nextButtonState, infoButtonState, quitButtonState, shufButtonState;
int  lastPlayButtonState, lastPrevButtonState, lastNextButtonState, lastInfoButtonState,
     lastQuitButtonState, lastShufButtonState;
long lastPlayDebounceTime, lastPrevDebounceTime, lastNextDebounceTime, lastInfoDebounceTime,
     lastQuitDebounceTime, lastShufDebounceTime;
long debounceDelay = 50;

const int numButtons = 6;

const int buttonPins[] = { playButtonPin, prevButtonPin, nextButtonPin, infoButtonPin, quitButtonPin, shufButtonPin };

/*
 * System stuff
 */

// For signal catching
static void die(int sig)
{
  // Insert any GPIO cleaning here.
  // TODO maybe try to unmount the usb stick or some other clean up here... maybe?
  lcdClear(lcdHandle);
  if (sig != 0 && sig != 2)
      (void)fprintf(stderr, "caught signal %d\n", sig);
  if (sig == 2)
      (void)fprintf(stderr, "Exiting due to Ctrl + C\n");
  exit(0);
}

// Wall everyone that system is shutting down
void wall(char *msg)
{
    char message[80];
    sprintf(message, "echo %s | wall", msg);
    system(message);
}

// Print usage
int usage(const char *progName)
{
  fprintf(stderr, "Usage: %s [OPTION] \n"
    "-pins (shows what pins to use for buttons) \n"
    "-dir [dir] \n"
    "-songs [MP3 files]\n"
    "-usb (this reads in any music found in /MUSIC)\n"
    "\t-halt (part of -usb\n"
    "       allows the program to halt the system after\n"
    "       the 'quit' button was pressed.)\n"
    "\t-shuffle (part of -usb; shuffles playlist)\n",
    progName);
  return EXIT_FAILURE;
}

/*
 * Mounting function; might use it in the future
 */

// TODO mountToggle is never used so might as well remove this function
// Mount (if cmd == 1, do not attempt to unmount)
int mountToggle(int cmd, char *dir_name)
{
  if (mount("/dev/sda1", dir_name, "vfat", MS_RDONLY | MS_SILENT, "") == -1)
  {
    // If it is already mounted; then unmount it.
    if (errno == EBUSY && cmd == 2)
    {
      umount2("/MUSIC", MNT_FORCE);
      return UNMOUNTED;
    }
    // Filesystem is already mounted so just return as mounted.
    else if (errno == EBUSY && cmd != 2)
      return FILES_OK;
    else
      return MOUNT_ERROR;
  }
  else
    return FILES_OK;
}

// Check to see if the USB flash was mounted or not.
// Mount (if cmd == 1, do not attempt to unmount)
int checkMount()
{
  if (mount("/dev/sda1", "/MUSIC", "vfat", MS_RDONLY | MS_SILENT, "") == -1)
  {
    if (errno == EBUSY) // EBUSY means the filesystem is already mounted so just return OK
      return FILES_OK;
    else
      return MOUNT_ERROR;
  }
  else
    return FILES_OK;
}

/*
 * Linked list / playlist functions
 *
 * Possible FIXME ... maybe convert this whole program into C++ and make the following a class...?
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

// Get song name from playlist with index
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
 * Creates playlist
 */

// If USB has been mounted, load in songs.
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
    // FIXME STAR STAR STAR!!!  Add a way to only read in mp3 files.
    while ((dir = readdir(d)) != NULL)
    {
      // 8 = normal file; non-directory
      if (dir->d_type == 8)
      {
        index = (index == 0 ? 1 : index);
        string = malloc(MAXDATALEN);
        if (string == NULL)
          perror("malloc: reReadPlaylist");
        strcpy(string, dir_name);
        strcat(string, "/");
        strcat(string, dir->d_name);
        playlist_add_song(index++, string, &new_playlist);
      }
    }
  }
  closedir(d);
  pthread_mutex_lock(&cur_song.pauseMutex);
  num_songs = index - 1;
  pthread_mutex_unlock(&cur_song.pauseMutex);
  return new_playlist;
}

// Convert linked list into array
void ll2array(playlist_t *playlistptr, char **arr)
{
  int i;
  playlist_node_t *cur;

  for (i = 0, cur = *playlistptr; cur != NULL; i++, cur = cur->nextptr)
  {
    if (cur->songptr != NULL)
      arr[i] = cur->songptr;
  }
}

// Shuffle / randomize playlist
playlist_t randomize(playlist_t cur_playlist)
{
  char *tmp[MAXDATALEN];
  char *string;
  playlist_t new_playlist;
  int index = 0;

  ll2array(&cur_playlist, tmp);
  srand((unsigned)time(NULL));
  if (num_songs > 1)
  {
    size_t i;
    for (i = 0; i < num_songs - 1; i++)
    {
      if (strlen(tmp[i]) > 3)
      {
        size_t j = i + rand() / (RAND_MAX / (num_songs - i) + 1);
        char *t = tmp[j];
        tmp[j] = tmp[i];
        tmp[i] = t;
      }
    }
  }
  // Convert array back into linked list.
  playlist_init(&new_playlist);
  index = 0;
  while (index < num_songs)
  {
    string = malloc(MAXDATALEN);
    if (string == NULL)
      perror("malloc: shuffle");
    strcpy(string, tmp[index]);
    playlist_add_song(index, string, &new_playlist);
    index++;
  }
  return new_playlist;
}

/*
 * Threading functions
 *
 * Functions for when buttons are pressed
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

void shuffleMe()
{
  pthread_mutex_lock(&cur_song.pauseMutex);
  cur_song.play_status = SHUFFLE;
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
      // Saving, changing, restoring a byte in the data
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
  // If there is no title to be found, set title to the song file name.
  if (strlen(cur_song.title) == 0)
    strcpy(cur_song.title, cur_song.base_filename);
  if (strlen(cur_song.artist) == 0)
    sprintf(cur_song.artist, "UNKNOWN");
  if (strlen(cur_song.album) == 0)
    sprintf(cur_song.album, "UNKNOWN");
  // Set the second row to be the artist by default.
  strcpy(cur_song.FirstRow_text, cur_song.title);
  strcpy(cur_song.SecondRow_text, cur_song.artist);
  mpg123_close(m);
  mpg123_delete(m);
  mpg123_exit();
  // The following two lines are just to see when the scrolling should pause
  strncpy(cur_song.scroll_FirstRow, cur_song.FirstRow_text, 15);
  strncpy(cur_song.scroll_SecondRow, cur_song.SecondRow_text, 16);
  return 0;
}

/*
 * LCD display functions
 */

// Non-scrolling - Top row
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
    // Have to set to 15 because of music note
    if (strlen(cur_song.FirstRow_text) < 15)
    {
      // New song; set the previous title
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

// Non-scrolling - Bottom row
int printLcdSecondRow()
{
  int flag = TRUE;

  if (strlen(cur_song.SecondRow_text) < 16)
  {
    lcdPosition(lcdHandle, 0, 1);
    lcdPuts(lcdHandle, cur_song.SecondRow_text);
    flag = FALSE;
    // New song; set the previous artist
    if (strcmp(cur_song.artist, cur_song.prevArtist) != 0)
      strcpy(cur_song.prevArtist, cur_song.artist);
  }
  return flag;
}

// Scrolling - Top row
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
  // Pause briefly when text reaches begining line before continuing
  *pauseScroll_FirstRow_Flag = (strcmp(buf, cur_song.scroll_FirstRow) == 0 ? TRUE : FALSE);
}

// Scrolling - Bottom row
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
  // Pause briefly when text reaches begining line before continuing
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
  // Try to not show error messages
  mh = mpg123_new(NULL, &err);
  mpar = mpg123_new_pars(&err);
  mpg123_par(mpar, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
  mh = mpg123_parnew(mpar, NULL, &err);
  buffer_size = mpg123_outblock(mh);
  buffer = (unsigned char*) malloc(buffer_size * sizeof(unsigned char));
  // Open the file and get the decoding format
  mpg123_open(mh, args->filename);
  mpg123_getformat(mh, &rate, &channels, &encoding);
  // Set the output format and open the output device
  format.bits = mpg123_encsize(encoding) * 8;
  format.rate = rate;
  format.channels = channels;
  format.byte_format = AO_FMT_NATIVE;
  format.matrix = 0;
  dev = ao_open_live(driver, &format, NULL);
  // Decode and play
  while (mpg123_read(mh, buffer, buffer_size, &done) == MPG123_OK)
  {
    checkPause();
    ao_play(dev, (char *) buffer, done);
    // Stop playing if the user pressed quit, shuffle, next, or prev buttons
    if (cur_song.play_status == QUIT || cur_song.play_status == NEXT || cur_song.play_status == PREV || cur_song.play_status == SHUFFLE)
      break;
  }
  // Clean up
  free(buffer);
  ao_close(dev);
  mpg123_close(mh);
  mpg123_delete(mh);
  mpg123_exit();
  ao_shutdown();
  pthread_mutex_lock(&(cur_song.writeMutex));
  args->song_over = TRUE;
  // Only set the status to play if the song finished normally
  if (cur_song.play_status != QUIT && cur_song.play_status != SHUFFLE && cur_song.play_status != NEXT && cur_song.play_status != PREV)
    args->play_status = PLAY;
  cur_status.song_over = TRUE; // FIXME only time cur_status is used?! Might just delete the entire struct...
  pthread_mutex_unlock(&(cur_song.writeMutex));
}

// Main function
int main(int argc, char **argv)
{
  pthread_t song_thread;
  playlist_t init_playlist;
  playlist_t cur_playlist;
  clock_t startPauseFirstRow;  // For pausing scroll display
  clock_t startPauseSecondRow; // For pausing scroll display
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
  int shuffFlag = FALSE;
  int playlistStatusErr = FILES_OK;
  int scroll_FirstRow_flag;
  int scroll_SecondRow_flag;
  int haltFlag = FALSE;
  int pauseScroll_FirstRow_Flag = FALSE;
  int pauseScroll_SecondRow_Flag = FALSE;
  int firstTime_FirstRow_Flag = TRUE;
  int firstTime_SecondRow_Flag = TRUE;
  int temp_FirstRow_Flag = FALSE;
  int temp_SecondRow_Flag = FALSE;

  // Initializations
  playlist_init(&cur_playlist);
  playlist_init(&init_playlist);
  ctrSecondRowScroll = 0;
  cur_song.song_over = FALSE;
  scroll_FirstRow_flag = scroll_SecondRow_flag = FALSE;
  lastPlayButtonState = lastPrevButtonState =
    lastNextButtonState = lastInfoButtonState =
    lastQuitButtonState = lastShufButtonState = HIGH;
  lastPlayDebounceTime = lastPrevDebounceTime =
    lastNextDebounceTime = lastInfoDebounceTime =
    lastQuitDebounceTime = lastShufDebounceTime = 0;
  // Use the following instead of delay
  startPauseFirstRow = clock();
  startPauseSecondRow = clock();
  if (argc > 1)
  {
    // Random/shuffle songs on startup
    for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-shuffle") == 0)
      {
        shuffFlag = TRUE;
        break;
      }
    }
    if (strcmp(argv[1], "-pins") == 0)
    {
      printf("Pins for buttons:\nFunction\twiringPi\tBCM\n"
             "--------\t--------\t---\n"
             "Play    \t0       \t17\n"
             "Prev    \t1       \t18\n"
             "Next    \t2       \t27\n"
             "Info    \t5       \t25\n"
             "Quit    \t7       \t4\n"
             "Shuffle \t11      \t7\n");
      return 1;
    }
    else if (strcmp(argv[1], "-songs") == 0)
    {
      for (index = 2; index < argc; index++)
      {
        string = malloc(MAXDATALEN);
        if (string == NULL)
          perror("malloc: -songs");
        strcpy(string, argv[index]);
        playlist_add_song(index - 1, string, &init_playlist);
        num_songs = argc - 2;
      }
      // FIXME I'm lazy right now; just threw this in so the test at the end
      // won't fail.
      playlistStatusErr = FILES_OK;
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
      // First, check to see if we need to halt
      if (argc == 3)
      {
        if (strcmp(argv[2], "-halt") == 0)
          haltFlag = TRUE;
      }
      // Secondly, check if USB is mounted.
      playlistStatusErr = checkMount();
      if (playlistStatusErr != MOUNT_ERROR)
      {
        if (playlistStatusErr == FILES_OK)
          init_playlist = reReadPlaylist("/MUSIC");
        if (num_songs == 0)
          playlistStatusErr = NO_FILES;
      }
    }
    else if (strcmp(argv[1], "-dir") == 0)
    {
      init_playlist = reReadPlaylist(argv[2]);
      if (num_songs == 0)
      {
        printf("No songs found in directory %s\n", argv[2]);
        return -1;
      }
      // FIXME I'm lazy right now; just threw this in so the test at the end
      // won't fail.
      playlistStatusErr = FILES_OK;
    }
    else if (shuffFlag == FALSE)
        return usage(argv[0]);
  }
  else
    return usage(argv[0]);

  // Catch signals
  // NOTE: We're assuming BSD-style reliable signals here
  (void)signal(SIGINT, die);
  (void)signal(SIGHUP, die);
  (void)signal(SIGTERM, die);
  if (wiringPiSetup () == -1)
  {
    fprintf(stdout, "oops: %s\n", strerror(errno));
    return 1;
  }
  lcdHandle = lcdInit(RO, CO, BS, RS, EN, D0, D1, D2, D3, D0, D1, D2, D3);
  if (lcdHandle < 0)
  {
    fprintf(stderr, "%s: lcdInit failed\n", argv[0]);
    return -1;
  }
  // Setup buttons
  for (i = 0; i < numButtons; i++)
  {
    pinMode(buttonPins[i], INPUT);
    pullUpDnControl(buttonPins[i], PUD_UP);
  }
  // Setup board test
  pinMode(boardTestPin, INPUT);
  pullUpDnControl(boardTestPin, PUD_UP);
  // Test to see if the display/buttons are attached.
  // CE0 is unused; so on the pcb attach CE0 to ground
  if (digitalRead(boardTestPin) == HIGH)
  {
    // TODO See if there's a better way to do this.
    //printf("LCD / buttons not found.\n");
    lcdClear(lcdHandle);
    lcdPosition(lcdHandle, 0, 0);
    lcdPuts(lcdHandle, "LCD not plugged in");
    lcdPosition(lcdHandle, 0, 1);
    if (haltFlag == TRUE)
    {
      //printf("Shutting down.\n");
      wall("LCD / buttons not found. Shutting down.");
      lcdPuts(lcdHandle, "Shutting down.");
      delay(1000);
      system("shutdown -h now");
    }
    else
    {
      lcdPuts(lcdHandle, "Please shutdown.");
      //printf("Please shutdown.\n");
      wall("LCD / buttons not found. Please shutdown.");
    }
  }
  if (playlistStatusErr == FILES_OK)
  {
    song_index = 1;
    if (shuffFlag == TRUE)
      cur_playlist = randomize(init_playlist);
    else
      cur_playlist = init_playlist;
    cur_song.play_status = PLAY;
    strcpy(cur_song.prevTitle, cur_song.title);
    strcpy(cur_song.prevArtist, cur_song.artist);
    /*
     * The below was once part of the while loop but I took it out so the playlist can loop.
     * TODO maybe in the future, add it as an option if you don't want it to loop?
     *
     *  && song_index < num_songs)
     */
    while (cur_song.play_status != QUIT)
    {
      // Loop playlist; reset song to begining of list
      if (song_index > num_songs)
        song_index = 1;
      string = malloc(MAXDATALEN);
      if (string == NULL)
        perror("malloc: song string");
      playlist_get_song(song_index, (void **) &string, &cur_playlist);
      if (string != NULL)
      {
        basec = strdup(string);
        bname = basename(basec);
        strcpy(cur_song.filename, string);
        strcpy(cur_song.base_filename, bname);
        // See if we can get the song info from the file.
        id3_tagger();
        // Play the song as a thread
        pthread_create(&song_thread, NULL, (void *) play_song, (void *) &cur_song);
        // The following displays stuff to the LCD without scrolling
        scroll_FirstRow_flag = printLcdFirstRow();
        scroll_SecondRow_flag = printLcdSecondRow();
        // Loop to play the song
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
                // Start the timer
                if (pauseScroll_FirstRow_Flag == TRUE && temp_FirstRow_Flag == FALSE)
                {
                  startPauseFirstRow = clock();
                  temp_FirstRow_Flag = TRUE;
                }
                if (temp_FirstRow_Flag == TRUE)
                {
                  // Check to see if 1 second has passed
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
          // Check to see if you just pressed the button 
          // (i.e. the input went from HIGH to LOW),  and you've waited 
          // long enough since the last press to ignore any noise:  
          // If the switch changed, due to noise or pressing:
          if (reading != lastPlayButtonState)
            lastPlayDebounceTime = millis(); // reset the debouncing timer
          if ((millis() - lastPlayDebounceTime) > debounceDelay)
          {
            // Whatever the reading is at, it's been there for longer
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
                  // Copy whatever is currently on the second row
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
          // Save the reading. Next time through the loop, it'll be the lastButtonState:
          lastPlayButtonState = reading;
          // Don't even check to see if the prev/next/info/quit/shuffle buttons
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
                  song_index = (song_index - 1 != 0 ? song_index - 1 : num_songs - 1);
                  prevSong();
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
                  song_index = (song_index + 1 < num_songs ? song_index + 1 : 1);
                  nextSong();
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
                  // Toggle what to display
                  strcpy(cur_song.SecondRow_text, (strcmp(cur_song.SecondRow_text, cur_song.artist) == 0 ? cur_song.album : cur_song.artist));
                  // First clear just the second row, then re-display the second row
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
            if (reading != lastQuitButtonState)
              lastQuitDebounceTime = millis();
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
            /*
             * Shuffle button
             */
            reading = digitalRead(shufButtonPin);
            if (reading != lastShufButtonState)
              lastShufDebounceTime = millis();
            if ((millis() - lastShufDebounceTime) > debounceDelay)
            {
              if (reading != shufButtonState)
              {
                shufButtonState = reading;
                if (shufButtonState == LOW)
                {
                  // Toggle shuffle state
                  // (NOTE: shuffFlag is useless here)
                  shuffFlag = (shuffFlag == TRUE ? FALSE : TRUE);
                  // The following function signals to go to next song
                  // and sets the play status to SHUFFLE
                  shuffleMe();
                }
              }
            }
            lastShufButtonState = reading;
          } // end ! pause
        } // end while
        // Reset all the flags.
        scroll_FirstRow_flag = scroll_SecondRow_flag = FALSE;
        pauseScroll_FirstRow_Flag = pauseScroll_SecondRow_Flag = FALSE;
        firstTime_FirstRow_Flag = firstTime_SecondRow_Flag = TRUE;
        temp_FirstRow_Flag = temp_SecondRow_Flag = FALSE;
        ctrSecondRowScroll = 0;
        if (pthread_join(song_thread, NULL) != 0)
          perror("join error\n");
        // Clear the lcd for next song.
        lcdClear(lcdHandle);
      }
      lcdClear(lcdHandle);
      // Increment the song_index if the song is over but the next/prev wasn't hit
      if (cur_song.song_over == TRUE && cur_song.play_status == PLAY)
      {
        pthread_mutex_lock(&cur_song.pauseMutex);
        cur_song.song_over = FALSE;
        pthread_mutex_unlock(&cur_song.pauseMutex);
        song_index++;
      }
      // Reset everything if next, prev, or shuffle buttons were pressed
      else if (cur_song.song_over == TRUE && (cur_song.play_status == NEXT || cur_song.play_status == PREV || cur_song.play_status == SHUFFLE))
      {
        pthread_mutex_lock(&cur_song.pauseMutex);
        // Empty out song/artist data
        strcpy(cur_song.title, "");
        strcpy(cur_song.artist, "");
        strcpy(cur_song.album, "");
        if (cur_song.play_status == SHUFFLE)
        {
          if (shuffFlag == TRUE)
            cur_playlist = randomize(init_playlist);
          else
            cur_playlist = init_playlist;
          song_index = 1;
        }
        cur_song.play_status = PLAY;
        cur_song.song_over = FALSE;
        pthread_mutex_unlock(&cur_song.pauseMutex);
      }
    }
    // Quit button was pressed
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
    // The following will never happen because the playlist loops now
    // TODO either remove it or add a possible "loop" flag option
    /*
    else
    {
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
    */
  }
  else if (playlistStatusErr == MOUNT_ERROR)
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
  else if (playlistStatusErr == NO_FILES)
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
  return 0;
}
