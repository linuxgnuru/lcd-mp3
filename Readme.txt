- NEWS -
After over a year, I thought I'd make another update.  I've decided to add the playback of ogg vorbis and maybe mp4. As I have a Pi 3 on it's way, I've decided to update the hardware as well to make it work on a B+/2/3 Raspberry Pi. As the B+/2/3 has more GPIO pins, I won't have to use I2C, UART, or SPI pins (in theory) which will mean I'll more than likely just make a branch of this however both will have the additional playback of ogg/mp4.

LCD MP3 player for Raspberry Pi

NOTE: In order to run the binary only, you'll need to install libao4 and libao-common.  In order to compile, you will need to install the following packages: libao libao-common libao-dev libmpg123 libmpg123-dev mpg321

You'll also need wiringPi which can be found here: http://wiringpi.com/download-and-install/

This is all from memory, so if I've missed anything, (i.e. it won't compile for you) please let me know so I can update this list.
