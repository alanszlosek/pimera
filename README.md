# PiMera

PiMera is security and critter camera application for Raspberry Pi computers and the Rasperry Pi camera modules.

If you would like to watch me build this project, check out my [PiMera YouTube playlist](https://www.youtube.com/watch?v=joc-nHM-NFU&list=PLGonE3T1sorRArqmtf22yUj0KgO2FpFvG).

I'm still getting comfortable with libcamera, but am hoping things will go faster once I can begin porting my Python code to C.

## Current Status

Developing and testing with:

* Raspberry Pi 3B+
* Pi Camera module v2
* PiOS version based on Debian 11
* Clang

### Working Features

* Requesting 2 frame sizes from libcamera at 30fps
  * 1920x1080 - will be saved to h264 eventually
  * 640x360 - for motion detection and MJPEG streaming. Smaller size reduces CPU usage during JPEG encoding.
* Motion detection 3x per second
* MJPEG streaming to browser at 10fps. Can go higher since 3B+ has a multi-core CPU.
* CPU utilization stays under 40% when streaming, lower when not

### Features In Progress

* Annotating frames with timestamp (almost done)
* Encoding to h264 and saving to disk when is motion detected

### Coming soon

* Configurable via INI file
* Response to HUP signal and reload config from INI


## History

This is not entirely a new project. I created a similar project in Python 3 many years ago, see the [raspi-hd-surveillance](https://github.com/alanszlosek/raspi-hd-surveillance) GitHub repository. However, Raspberry Pi is [moving away](https://www.raspberrypi.com/documentation/accessories/camera.html) from the proprietary Broadcom camera stack to the open source [libcamera](https://libcamera.org) library, so now seemed like a good time to re-think things. Plus, I enjoy writing in C (C++ not so much).

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
