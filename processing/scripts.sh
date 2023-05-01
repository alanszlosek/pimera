#!/bin/bash
cd /home/user/projects/pimera
./01.copy.sh
python3 ./02.convert.py /mnt/media/surveillance
./03.index.sh
