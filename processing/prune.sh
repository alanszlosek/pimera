#!/bin/bash
# BE SURE TO UPDATE ../config.json WITH THE CORRECT VALUES FOR YOUR SETUP

# change into this directory
cd -- "$(dirname "$0")"

if [[ ! -d "./venv" ]]; then
    python3 -m venv ./venv
    source venv/bin/activate
    pip install mysql-connector-python ultralytics opencv-python
else
    source venv/bin/activate
fi

python3 -u ./06.prune-mark.py || exit 1
python3 -u ./07.prune-files.py || exit 1
python3 -u ./08.prune-rows.py || exit 1

