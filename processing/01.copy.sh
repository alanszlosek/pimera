#!/bin/bash
HOSTS=$(cat hosts.txt)
DESTINATION=$1
#cd /mnt/media/surveillance/h264


date
echo "COPYING THEN REMOVING"

for ip in ${HOSTS}
do
	# Make list of files to copy so we can be more targeted in removal.
	# If we do "rm *.h264" as last step, it may remove files
	# that were created while we were copying, which means files will be lost.
	ssh "pi@${ip}" "cd ~/h264; find *.h264" > ./h264_files.txt

	# Do the copy, then remove the files we copied
	echo "copying to ${DESTINATION}"
	rsync -v --files-from=./h264_files.txt "pi@${ip}:~/h264/" "${DESTINATION}/" && ssh "pi@${ip}" "cd ~/h264; rm "$(cat ./h264_files.txt)""

done

