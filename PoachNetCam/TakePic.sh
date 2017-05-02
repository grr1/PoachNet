#!/bin/bash

# --------TakePic.sh-------- 
# Coordinates PoachNet operations then shuts down the raspi.


sleep 5

#Variables.
GPIO_DEV=29      #Determines Dev mode.
GPIO_MAS=28      #Determines if this raspi is "Master" of its cluster.
#GPIO_WAK=8      #Wakes up other raspis.
ROOT_POACHNET='/home/pi/PoachNet'
LOG='/home/pi/PoachNet/shutdown_log'
SERVER='https://poachnetcs.mybluemix.net/uploader'
#SERVER='http://poachnet.duckdns.org:3000/iot_post'
WAIT_FOR_RASPI=30
WAIT_FOR_WIFI=10

# gpio mode $GPIO_WAK out
gpio mode $GPIO_DEV in
gpio mode $GPIO_MAS in

state=1
#state=$(gpio read $GPIO_DEV)
date=$(date +"%Y-%m-%d_%H-%M-%S")


function swap_interfaces_to_master()
{
	if [ ! -f $ROOT_POACHNET/MasterMode ]
	then
    		sudo cp $ROOT_POACHNET/interfaces_master /etc/network/
		sudo mv /etc/network/interfaces_master /etc/network/interfaces
		sudo ifdown wlan0 && sudo ifup wlan0 >> /dev/null 2>&1
		touch $ROOT_POACHNET/MasterMode
			
		if [ -f $ROOT_POACHNET/CameraMode ]
		then
			rm $ROOT_POACHNET/CameraMode
		fi
	fi
}



function swap_interfaces_to_camera()
{
	if [ ! -f $ROOT_POACHNET/CameraMode ]
	then
   		sudo cp $ROOT_POACHNET/interfaces_camera /etc/network/ >> $LOG 2>&1
		sudo mv /etc/network/interfaces_camera /etc/network/interfaces >> $LOG 2>&1
		sudo ifdown wlan0 && sudo ifup wlan0 >> /dev/null 2>&1
		touch $ROOT_POACHNET/CameraMode
		
		if [ -f $ROOT_POACHNET/MasterMode ]
		then
			rm $ROOT_POACHNET/MasterMode
		fi
	fi
}



function verify_internet() {

	echo "$date:  network test." >> $LOG wget -q --spider http://google.com
	ret=$?

	for (( i=1; i<=5; i++ ))
	do
		
		if [ $ret -eq 0 ]
		then
    			echo "$date:  network verified." >> $LOG
			return 0
		else
			echo "$date:  network not up. trying again." >> $LOG
    			sleep 5
			sudo ifdown wlan0 && sudo ifup wlan0 >> /dev/null 2>&1
			wget -q --spider http://google.com
			ret=$?
		fi
	done

	echo "$date:  network bad." >> $LOG
	return 1
}





if [ $state -eq 1 ]
then
	#Dev mode, do not shutdown pi.
	
	echo "$date:  not shutting down." >> $LOG
	state=$(gpio read $GPIO_MAS)

	if [ $state -eq 1 ]
	then
		echo "$date:  [DEV] master mode." >> $LOG
		echo "$date:  starting PI_AP." >> $LOG

		swap_interfaces_to_master
		verify_internet
		ret=$?
		
		if [ $ret -eq 0 ]
		then
			echo "$date:  attempting to start hostapd." >> $LOG
			sudo /etc/init.d/hostapd start >> $LOG 2>&1
			echo "$date:  attempting to start dnsmasq." >> $LOG
			sudo /etc/init.d/dnsmasq start >> $LOG 2>&1
		fi
	else
		echo "$date:  [DEV] camera mode." >> $LOG
		echo "$date:  connecting to PI_AP." >> $LOG

		sleep $WAIT_FOR_WIFI
		
		swap_interfaces_to_camera
		verify_internet
	fi
	
	printf "\n\n" >> $LOG
	
else
	#gpio pin is low: shutdown pi after operations.
	state = 1	
	#state=$(gpio read $GPIO_MAS)
	raspistill -o "$ROOT_POACHNET/pics/$date.jpg"                  #Take picture.
	
	if [ $state -eq 1 ]
	then
		echo "$date:  master mode." >> $LOG

		echo "$date:  starting PI_AP." >> $LOG
		swap_interfaces_to_master
		verify_internet
		ret=$?
		
		if [ $ret -eq 0 ]
		then
			echo "$date:  attempting to start hostapd." >> $LOG
			sudo /etc/init.d/hostapd start >> $LOG 2>&1
			echo "$date:  attempting to start dnsmasq." >> $LOG
			sudo /etc/init.d/dnsmasq start >> $LOG 2>&1
		fi
		
		echo "$date:  sending image." >> $LOG
		curl -v -F "tag=oyster" -F "image=@$ROOT_POACHNET/pics/$date.jpg" $SERVER >> $LOG 2>&1     #Send to poachnet server.

		#Wait for other Raspis.
		sleep $WAIT_FOR_RASPI
	else
		echo "$date:  camera mode." >> $LOG

		sleep $WAIT_FOR_WIFI

		echo "$date:  connecting to PI_AP." >> $LOG		
		swap_interfaces_to_camera
		verify_internet
		
		echo "$date:  sending image." >> $LOG
		curl -v -F "tag=oyster" -F "image=@$ROOT_POACHNET/pics/$date.jpg" $SERVER >> $LOG 2>&1   #Send to poachnet server.
	fi
	
	echo "$date:  shutting down." >> $LOG
	printf "\n\n" >> $LOG
	sudo shutdown -h now
fi
