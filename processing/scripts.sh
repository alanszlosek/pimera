#!/bin/bash
# BE SURE TO UPDATE ../config.json WITH THE CORRECT VALUES FOR YOUR SETUP

# RUN ALL PROCESSING SCRIPTS

# change into this directory
cd -- "$(dirname "$0")"

if [[ ! -d "./venv"]]; then
    python3 -m venv ./venv
    pip install mysql-connector-python ultralytics Pillow
fi
source venv/bin/activate

./01.copy.sh
python3 ./02.convert.py /mnt/media/surveillance
python3 ./03.index.py
python3 ./04.thumbnails.py
python3 ./05.detection.py
