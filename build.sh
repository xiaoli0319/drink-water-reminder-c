#!/bin/bash
gcc main.c $(pkg-config --cflags --libs x11 xft dbus-1 libpng) -o drink-reminder
strip drink-reminder
echo "Binary size: $(stat -c%s drink-reminder) bytes"
