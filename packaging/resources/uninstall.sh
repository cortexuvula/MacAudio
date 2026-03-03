#!/bin/bash
# MacAudio Uninstaller
# Double-click this file to uninstall MacAudio and its audio driver.

echo "=== MacAudio Uninstaller ==="
echo ""
echo "This will remove MacAudio.app and the MacAudio audio driver."
echo "You will be asked for your administrator password."
echo ""
read -p "Continue? (y/N) " confirm
if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
    echo "Cancelled."
    exit 0
fi

echo ""
echo "Removing MacAudio..."

sudo rm -rf "/Applications/MacAudio.app"
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/MacAudioDriver.driver"

# Gracefully restart coreaudiod so it unloads the driver
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || \
  sudo killall coreaudiod 2>/dev/null

echo ""
echo "MacAudio has been uninstalled."
echo "Press any key to close."
read -n 1
