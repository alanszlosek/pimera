import glob
import multiprocessing
import os
import pathlib
import re
import subprocess
import sys



def task1(files):
    for source in files:
        task2(source)

def task2(source):
    dims = re.search(r'(\d{8})\d{6}_([a-z0-9]+)_(\d+)x(\d+)x(\d+)\.h264$', source)
    if dims:
        fps = int(dims.group(5))
    else:
        return "Failed to parse filename: %s" % (source,)

    # files are stored like h264/20210101000000_raspi3_1920x1080x30.h264
    # convert and move them to: mp4/20210101000000_raspi3_1920x1080x30.mp4
    destination = source.replace('/h264/', '/mp4/').replace('.h264', '.mp4')
    destinationDir = os.path.dirname(destination)

    args = ['ffmpeg', '-framerate', str(fps), '-i', source, '-c:v', 'copy', '-y', destination]
    result = subprocess.run(args, capture_output=True, text=True) #, cwd=os.getcwd())
    if result.returncode == 0:
        os.unlink(source)
        return 'Done converting @ %s fps, removed %s' % (fps,source)
    return 'Failed to convert %s. %s' % (destination, result.stderr)

def chunks(lst, n):
    out = []
    for i in range(0, len(lst), n):
        yield lst[i:i + n]

if __name__ == '__main__':

    parentPath = sys.argv[1]
    path = os.path.join(parentPath, 'h264') + '/*.h264'
    print("Converting h264 to mp4")
    print(path)

    files = glob.glob(path)
    #chunked = chunks(files, len(files) / 2)
    #print( list(chunked))

    #for source in files:
    #    print( task(source) )

    with multiprocessing.Pool(processes=5) as p:
        results = p.map(func=task2, iterable=files, chunksize=1)
        print(results)


