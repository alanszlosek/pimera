#!/bin/bash
set -e
# BE SURE TO UPDATE ../config.json WITH THE CORRECT VALUES FOR YOUR SETUP

# RUN ALL PROCESSING SCRIPTS

# change into this directory
cd -- "$(dirname "$0")"

if [[ ! -d "./venv" ]]; then
    python3 -m venv ./venv
    source venv/bin/activate
    #pip install mysql-connector-python ultralytics Pillow opencv-python
    pip install mysql-connector-python ultralytics opencv-python requests
else
    source venv/bin/activate
fi


VIDEO_PATH=$(cat ../config.json | jq -r .videoPath)
./01.copy.sh "${VIDEO_PATH}" || exit 1
python3 -u ./02.convert.py "${VIDEO_PATH}" || exit 1
python3 -u ./02.organize.py "${VIDEO_PATH}" || exit 1
python3 -u ./03.index.py || exit 1
python3 -u ./04.detection.py || exit 1

