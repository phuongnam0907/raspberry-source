#!/bin/bash
ifconfig eth0 up;
udhcpc;
dropbear start;
ifconfig;
