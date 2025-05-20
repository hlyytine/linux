#! /bin/sh

while :; do
	if test -e /home/ubuntu/do_update; then
		tar -C / -xvf /home/ubuntu/update.tar
		rm -f /home/ubuntu/do_update
	fi
	sleep 0.5
done
