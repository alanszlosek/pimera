#!/bin/bash
if [[ ! -d venv ]]; then
    python3 -m venv ./venv
    source venv/bin/activate
    pip install flask
else
    source venv/bin/activate
fi
#env FLASK_APP=api.py FLASK_ENV=development flask run
python3 -u api.py

