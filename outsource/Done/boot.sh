#!/bin/bash
if [ $1 == '' ];
then
    echo "Input file .bin";
else
    echo 27 > /sys/class/gpio/export;
    echo 26 > /sys/class/gpio/export;
    echo out > /sys/class/gpio/gpio27/direction;
    echo out > /sys/class/gpio/gpio26/direction;
    echo 0 > /sys/class/gpio/gpio26/value;
    echo 0 > /sys/class/gpio/gpio27/value;
    sleep 1;
    echo 1 > /sys/class/gpio/gpio27/value;
    ./lpc21isp -bin $1 /dev/ttymxc2 115200 30000;
    echo 27 > /sys/class/gpio/unexport;
    echo 26 > /sys/class/gpio/unexport;
fi

#Testing LPC firmware
#./picocom -b 57600 /dev/ttymxc2 
#
#./BLF2App_VendingHostSimulator /dev/ttymxc2 57600
#
