#!/bin/bash
HOSTS=$(cat hosts.txt)
DESTINATION=$1

date
echo "COPYING THEN REMOVING"

for ip in ${HOSTS}
do
	# Make a list of files to copy so we can be more targeted in removal.
	# If we do a general "rm *.h264" as last step, it may remove files
	# that were created while we were copying, which means files will be lost.
	ssh "pi@${ip}" "cd ~/h264; find *.h264" > ./h264_files.txt
	# Clean up from previous run
	rm -f h264_batch*
	# If there are files to process
	if [[ -s h264_files.txt ]]; then
		# Split h264_files.txt into h264_batch01, h264_batch02, etc
		split -l 1000 --numeric-suffixes ./h264_files.txt h264_batch

		# Do the copy, then remove the files we copied
		echo "Copying to ${DESTINATION}"
		for batch in h264_batch*
		do
			BATCH_FILES=$(cat ${batch})
			echo "rsyncing then removing:"
			echo ${BATCH_FILES}
			# Use rsync to copy
			# Preserve timestamps to prevent duplicate copies if the script
			# fails for some reason.
			# Then remove the files rsync copied, but only if rsync succeeds
			rsync -tv --files-from=${batch} "pi@${ip}:~/h264/" "${DESTINATION}/" && ssh "pi@${ip}" "cd ~/h264; rm "$BATCH_FILES""
		done
	fi

done

