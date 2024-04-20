#!/bin/bash
set -e

HOSTS=$(cat hosts.txt)
HOST_USERNAME=pi
DESTINATION=$1

date
echo "COPYING TO '${DESTINATION}' THEN REMOVING"

for ip in ${HOSTS}
do
	for ext in h264 mp4
	do
		# Make a list of files to copy so we can be more targeted in removal.
		# If we do a general "rm *.h264" as last step, it may remove files
		# that were created while we were copying, which means files will be lost.
		# Use find . -name '*.ext' to avoid shell expansion when there are many files
		ssh "${HOST_USERNAME}@${ip}" "cd ~/${ext}; find . -name '*.${ext}'" > ./all_files
		# Clean up from previous run
		rm -f file_batch*
		# If there are files to process
		# If file exists and is non-empty
		if [[ -s all_files ]]; then
			# Split h264_files.txt into h264_batch01, h264_batch02, etc
			split -l 1000 --numeric-suffixes ./all_files file_batch

			# Do the copy, then remove the files we copied
			echo "Copying to ${DESTINATION}"
			for batch in file_batch*
			do
				BATCH_FILES=$(cat ${batch})
				echo "rsyncing then removing:"
				echo ${BATCH_FILES}
				# Use rsync to copy
				# Preserve timestamps to prevent duplicate copies if the script
				# fails for some reason.
				# Then remove the files rsync copied, but only if rsync succeeds
				rsync -tv --files-from=${batch} "${HOST_USERNAME}@${ip}:${ext}/" "${DESTINATION}/${ext}/" && ssh "${HOST_USERNAME}@${ip}" "cd ~/${ext}; rm "$BATCH_FILES"" done
		fi
	done

done

