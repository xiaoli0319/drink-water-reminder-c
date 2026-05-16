#!/bin/bash
gcc main.c $(pkg-config --cflags --libs x11 dbus-1) -o drink-reminder
strip drink-reminder
echo "Binary size: $(stat -c%s drink-reminder) bytes"
