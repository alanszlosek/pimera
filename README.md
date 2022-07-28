# PiMera

PiMera is security and critter camera application for Raspberry Pi computers and the Rasperry Pi camera modules.

If you would like to watch me build this project, check out my [PiMera YouTube playlist](https://www.youtube.com/watch?v=joc-nHM-NFU&list=PLGonE3T1sorRArqmtf22yUj0KgO2FpFvG).

I'm still getting comfortable with libcamera, but am hoping things will go faster once I can begin porting my Python code to C.

## History

This is not entirely a new project. I created a similar project in Python 3 many years ago, see the [raspi-hd-surveillance](https://github.com/alanszlosek/raspi-hd-surveillance) GitHub repository. However, Raspberry Pi is [moving away](https://www.raspberrypi.com/documentation/accessories/camera.html) from the proprietary Broadcom camera stack to the open source [libcamera](https://libcamera.org) library, so now seemed like a good time to re-think things. Plus, I enjoy writing in C (C++ not so much).

# Features

* Motion detection
    * Can exclude regions of the frame
    * Motion detected when N% of pixels have changed (configurable)
* Live streaming to browser. MJPEG twice a second.
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
