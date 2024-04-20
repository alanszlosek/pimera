import glob
import os
import pathlib
import re
import subprocess
import sys

parentPath = sys.argv[1]
path = os.path.join(parentPath, 'mp4') + '/*.mp4'
print(path)
print("Moving /mp4/*.mp4 to dated subdirectories")

for source in glob.glob(path):
    dims = re.search(r'(\d{8})\d{6}_([a-z0-9]+)_(\d+)x(\d+)x(\d+)\.mp4$', source)
    if dims:
        day = dims.group(1)
    else:
        print("Failed to parse filename: %s" % (source,))
        continue

    # files are stored like /mp4/20210101000000_raspi3_1920x1080x30.mp4
    # convert and move them to: /20210101/20210101000000_raspi3_1920x1080x30.mp4
    destination = source.replace('/mp4/', f"/{day}/")
    destinationDir = os.path.dirname(destination)
    pathlib.Path(destinationDir).mkdir(parents=True, exist_ok=True)
    os.rename(source, destination)


