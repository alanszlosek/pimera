#!/bin/bash
# BE SURE TO UPDATE ../config.json WITH THE CORRECT VALUES FOR YOUR SETUP
# AND THE PATHS BELOW

# RUN ALL PROCESSING SCRIPTS

# change into this directory
cd -- "$(dirname "$0")"

if [[ ! -d "./venv" ]]; then
    python3 -m venv ./venv
    source venv/bin/activate
    pip install mysql-connector-python ultralytics Pillow opencv-python
else
    source venv/bin/activate
fi

./01.copy.sh /mnt/media/surveillance/h264/ || exit 1
python3 ./02.convert.py /mnt/media/surveillance || exit 1
python3 ./03.index.py || exit 1
python3 ./04.detection.py || exit 1

