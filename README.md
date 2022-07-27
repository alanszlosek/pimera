# PiMera

NOTE: This is a work in progress. Not fully working yet.

A security and critter camera application for Raspberry Pi computers and the Rasperry Pi camera modules.

This is not entirely a new project. I created a similar project in Python 3 many years ago, see [raspi-hd-surveillance](https://github.com/alanszlosek/raspi-hd-surveillance).However, Raspberry Pi is [moving away](https://www.raspberrypi.com/documentation/accessories/camera.html) from the proprietary Broadcom camera stack to the open source [libcamera](https://libcamera.org) library, so now seemed like a good time to re-think things. Plus, I enjoy writing in C (C++ not so much).

I'm still getting comfortable with libcamera, but am hoping things will go faster once I can begin porting my Python code to C.


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
* In `src/`, run `make`
* Code has hardcoded defaults. Haven't implemented loading settings from file yet.
