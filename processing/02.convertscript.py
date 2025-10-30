import glob
import os
import pathlib
import re
import sys


if __name__ == '__main__':

    parentPath = sys.argv[1]
    pathDir = os.path.join(parentPath, 'h264')
    path = os.path.join(parentPath, 'h264') + '/*.h264'
    print("Converting h264 to mp4")
    print(path)

    files = glob.glob(path)

    pat = re.compile(r'(\d{8})\d{6}_([a-z0-9]+)_(\d+)x(\d+)x(\d+)\.h264$')

    cutoff = 4
    with open("generated.convert.sh", "w") as f:
        i = 0
        for source in files:
            if i == cutoff:
                i = 0
                f.write("wait\n")
                f.write("date\n")

            dims = pat.search(source)
            if dims:
                fps = str(int(dims.group(5)))
            else:
                print(f"Failed to parse filename: {source}\n")
                continue

            # files are stored like h264/20210101000000_raspi3_1920x1080x30.h264
            # convert and move them to: mp4/20210101000000_raspi3_1920x1080x30.mp4
            destination = source.replace('/h264/', '/mp4/').replace('.h264', '.mp4')
            destinationDir = os.path.dirname(destination)

            cmd = f"ffmpeg -framerate {fps} -i '{source}' -c:v copy -y -movflags +faststart '{destination}' &> /dev/null && rm '{source}' &> /dev/null &\n"
            #cmd = f"ffmpeg -framerate {fps} -i '{source}' -c:v copy -y -movflags +faststart '{destination}' && rm '{source}' &\n"
            f.write( cmd )

            i = i + 1

        f.write("wait\n")
        f.write(f"date\n")

        f.write("echo 'UNCONVERTED FILES'\n")
        f.write(f"ls {pathDir}\n")

        # now get list of files still in h264, list them and say "failed to convert these"


    print("Done")


