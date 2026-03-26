#!/bin/sh

make -C aesd-char-driver/ clean
sudo ./aesd-char-driver/aesdchar_unload
make -C aesd-char-driver/
sudo ./aesd-char-driver/aesdchar_load
