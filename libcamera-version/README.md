Pay no attention to this folder for the time being. I'm still actively working on the MMAL version.




## libcamera version

If you would like to watch me build this project, check out my [PiMera YouTube playlist](https://www.youtube.com/watch?v=joc-nHM-NFU&list=PLGonE3T1sorRArqmtf22yUj0KgO2FpFvG).

I'm still getting comfortable with libcamera, but am hoping things will go faster once I can begin porting my Python code to C.


# Features

* Motion detection
    * Can specify which regions of the frame to check for motion (allows you to exclude swaying trees, etc)
    * Motion detected when N% of pixels have changed (configurable)
* Live streaming to browser using MJPEG format
* When motion is detected, record to h264 files (portable-ish)
* Striving for efficient code that can support 1080p 30fps

# Installation

## Pre-requisites

* Raspberry Pi 3B+ or 4B (haven't tested)
* Raspberry Pi OS, based on Debian 11 or later
* apt install clang libcamera-dev libjpeg62-turbo-dev

## Building

* In `src/`, run `make`
* Code has hardcoded defaults. Haven't implemented loading settings from file yet.

# Using libcamera

If you're new to libcamera like I am, check out the `examples/libcamera-scaffold` directory. There you will find `scaffold.cpp` and a `Makefile` for building (possibly) the simplest possible libcamera app.

It configures the camera, registers a callback, receives frames via the callback function, then exits after 10 seconds.
