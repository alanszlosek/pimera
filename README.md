# PiMera

A security and critter camera application for Raspberry Pi computers and the Rasperry Pi camera modules.

NOTE: This is a work in progress. Not fully working yet.

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
