#!/bin/bash
HOSTS=$(cat hosts.txt)
cd /mnt/media/surveillance/h264

date
echo "COPYING THEN REMOVING"

for ip in ${HOSTS}
do
	#scp -r "pi@${ip}:~/h264/*.h264" "./" && ssh "pi@${ip}" "rm ~/h264/*.h264"
	rsync -v "pi@${ip}:~/h264/*.h264" "./" && ssh "pi@${ip}" "rm ~/h264/*.h264"
done


