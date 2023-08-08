This is the code for the current version of PiMera that I'm using. It uses the MMAL media APIs, so you'll need to enable them in your PiOS ("legacy camera").

## Performance Tuning

I did lots of code optimizations over the past couple months. Here's the status of the settings I'm using.

### Pi 3B+

* I swapped in a larger heatsink from AdaFruit.
* 1640 x 1232 @ 20 fps
* Pimera using 3% CPU
* CPU Temperature: 56 Celsius


### Pi Zero W

* Still need to move to a better case and install heatsink
* 1640 x 922 @ 10 fps (want to match the Pi 3B+)
* Pimera using 5% CPU
* CPU Temperature: 48 Celsius
