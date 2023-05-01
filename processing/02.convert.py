import glob
import os
import pathlib
import re
import subprocess
import sys

parentPath = sys.argv[1]
path = os.path.join(parentPath, 'h264') + '/*.h264'
print(path)

for source in glob.glob(path):
    dims = re.search(r'(\d{8})\d{6}_([a-z0-9]+)_(\d+)x(\d+)x(\d+)\.h264$', source)
    if dims:
        fps = int(dims.group(5))
        day = dims.group(1)
    else:
        print("Failed to parse filename: %s" % (source,))
        continue

    # files are stored like h264/20210101000000_raspi3_1920x1080x30.h264
    # convert and move them to: /20210101/20210101000000_raspi3_1920x1080x30.mp4
    destination = source.replace('/h264/', '/%s/' %(day,)).replace('.h264', '.mp4')
    destinationDir = os.path.dirname(destination)
    pathlib.Path(destinationDir).mkdir(parents=True, exist_ok=True)

    args = ['ffmpeg', '-framerate', str(fps), '-i', source, '-c:v', 'copy', '-y', destination]
    result = subprocess.run(args, capture_output=True, text=True) #, cwd=os.getcwd())
    if result.returncode == 0:
        print('Done converting @ %s fps, now removing %s' % (fps,source))
    else:
        print('Failed to convert %s. %s' % (destination, result.stderr))
    os.unlink(source)

