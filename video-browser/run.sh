#!/bin/bash
if [[ ! -d venv ]]; then
    python3 -m venv ./venv
    source venv/bin/activate
    pip install mysql-connector-python
else
    source venv/bin/activate
fi
python3 -u api.py

