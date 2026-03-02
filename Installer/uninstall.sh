#!/bin/bash
set -e

DRIVER_NAME="MacAudioDriver.driver"
HAL_DIR="/Library/Audio/Plug-Ins/HAL"

if [ ! -d "$HAL_DIR/$DRIVER_NAME" ]; then
    echo "$DRIVER_NAME is not installed."
    exit 0
fi

echo "Removing $DRIVER_NAME from $HAL_DIR..."
sudo rm -rf "$HAL_DIR/$DRIVER_NAME"
echo "Restarting coreaudiod..."
sudo killall coreaudiod
echo "Done. MacAudio Virtual Device has been removed."
