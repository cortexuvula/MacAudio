#!/bin/bash
set -e

DRIVER_NAME="MacAudioDriver.driver"
HAL_DIR="/Library/Audio/Plug-Ins/HAL"
BUILD_DIR="$(dirname "$0")/../build/Debug"

if [ ! -d "$BUILD_DIR/$DRIVER_NAME" ]; then
    echo "Error: $DRIVER_NAME not found in $BUILD_DIR"
    echo "Please build the project first (Cmd+B in Xcode)"
    exit 1
fi

echo "Installing $DRIVER_NAME to $HAL_DIR..."
sudo rm -rf "$HAL_DIR/$DRIVER_NAME"
sudo cp -R "$BUILD_DIR/$DRIVER_NAME" "$HAL_DIR/"
echo "Restarting coreaudiod..."
sudo killall coreaudiod
echo "Done. 'MacAudio Virtual Device' should now appear in System Settings > Sound > Input."
