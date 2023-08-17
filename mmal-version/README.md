This is the code for the current version of PiMera that I'm using. It uses the MMAL media APIs, so you'll need to enable them in your PiOS ("legacy camera").

## Performance Tuning

I did lots of code optimizations over the past couple months. Here's the status of the settings I'm using.

### Pi 3B+

* I swapped in a larger heatsink from AdaFruit.
* 1640 x 1232 @ 20 fps
* Pimera using 3% CPU
* CPU Temperature: 57 Celsius

| Case | Heatsink | Resolution | FPS | CPU Celsius | CPU Utilization |
| --- | --- | --- | --- | --- | --- |
| Clear w/Camera | 3mm | 1640 x 1232 | 20 | 60 | 4% |
| Clear w/Camera | 14x14x8mm | 1640 x 1232 | 20 | 58 | 5% |

### Pi Zero W

* Still need to move to a better case and install heatsink

Grr, Pi Zero W just keeps hanging. Not sure why. Temperature seems fine.

| Case | Heatsink | Resolution | FPS | CPU Celsius | CPU Utilization |
| --- | --- | --- | --- | --- | --- |
| Official w/Camera | None | 1640 x 922 | 10 | 49 | 4% |
| Official w/Camera | None | 1640 x 1232 | 10 | 51 | 5% |
| PiHut case | 14x14x8mm | 1640 x 1232 | 10 | 49 | 5% |
| PiHut case | 14x14x8mm | 1640 x 1232 | 15 | 53 | 6% |
| PiHut case | 14x14x8mm | 1640 x 1232 | 20 | 53 | 8% |

"Official w/Camera" means the Official Raspbery Pi Zero case with the camera lid.

"CPU utilization" refers to CPU used by pimera as reported in top.

### Official Pi case without heatsink

* CPU Temperature: 48 Celsius

### Diff case and bla heatsink