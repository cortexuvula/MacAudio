#!/bin/bash
set -euo pipefail

# Run xcodegen
echo "Running xcodegen..."
xcodegen generate

# Restore entitlements (xcodegen wipes to empty dict)
echo "Restoring MacAudio.entitlements..."
cat > MacAudio/MacAudio.entitlements << 'ENTITLEMENTS'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.device.audio-input</key>
    <true/>
</dict>
</plist>
ENTITLEMENTS

# Restore CFBundleExecutable in driver Info.plist (xcodegen strips it)
echo "Restoring MacAudioDriver/Info.plist CFBundleExecutable..."
/usr/libexec/PlistBuddy -c "Add :CFBundleExecutable string '\$(EXECUTABLE_NAME)'" \
    MacAudioDriver/Info.plist 2>/dev/null || \
/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable '\$(EXECUTABLE_NAME)'" \
    MacAudioDriver/Info.plist

echo "Done. Xcode project regenerated with fixes applied."
