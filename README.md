# MacAudio

A macOS menu bar app that captures microphone and system audio, mixes them together, and exposes the mix as a virtual input device. Any app that accepts audio input (QuickTime, Zoom, OBS, Discord, etc.) can select "MacAudio Virtual Device" to receive the combined stream.

Requires **macOS 14.2+** (Sonoma) — uses the Core Audio Taps API for system audio capture.

## How It Works

```
Mic (CoreAudio IOProc) ──┐
                          ├─→ AudioMixer ──→ SharedRingBuffer (shm) ──→ HAL Driver ──→ Virtual Input Device
System (CATapDescription) ┘
```

The app and driver run in separate processes. The Swift app captures and mixes audio, then writes interleaved Float32 samples to a POSIX shared memory ring buffer. The C driver (an AudioServerPlugin) reads from the same ring buffer and presents the data as a standard macOS input device.

## Requirements

- macOS 14.2 or later
- [Xcode](https://developer.apple.com/xcode/) 16.0+
- [xcodegen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`)
- Developer ID certificate for code signing (required on macOS 15+ for HAL driver loading)

## Permissions

The app requests the following permissions on first launch:

- **Microphone** — to capture mic input
- **Screen & System Audio Recording** — to capture system audio via `AudioHardwareCreateProcessTap`

Grant these in **System Settings > Privacy & Security**.

## Building

### Generate the Xcode project

```bash
xcodegen generate
```

> **Note:** After every `xcodegen generate`, you must manually restore:
> 1. `MacAudio/MacAudio.entitlements` — xcodegen resets it. Restore the `com.apple.security.device.audio-input` entitlement.
> 2. `MacAudioDriver/Info.plist` — xcodegen strips `CFBundleExecutable`. Re-add it.

### Build the app (includes driver)

```bash
xcodebuild -project MacAudio.xcodeproj -target MacAudio -configuration Debug build \
  CODE_SIGN_IDENTITY="Developer ID Application" DEVELOPMENT_TEAM=RB4QV9W52C CODE_SIGN_STYLE=Manual
```

### Build the driver only

```bash
xcodebuild -project MacAudio.xcodeproj -target MacAudioDriver -configuration Debug clean build \
  CODE_SIGN_IDENTITY="Developer ID Application" DEVELOPMENT_TEAM=RB4QV9W52C CODE_SIGN_STYLE=Manual
```

## Installing the Driver

### Using the install script

```bash
./Installer/install.sh
```

### Manual install

```bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/MacAudioDriver.driver
sudo cp -R build/Debug/MacAudioDriver.driver /Library/Audio/Plug-Ins/HAL/
sudo xattr -rc /Library/Audio/Plug-Ins/HAL/MacAudioDriver.driver
sudo killall coreaudiod
```

Verify the driver is loaded:

```bash
system_profiler SPAudioDataType | grep -A5 "MacAudio"
```

### Uninstall

```bash
./Installer/uninstall.sh
```

## Usage

1. Install the driver (see above)
2. Launch MacAudio — it appears as a menu bar icon
3. Click the icon and press **Start**
4. In any recording app, select **MacAudio Virtual Device** as the input device
5. Use the sliders to adjust mic and system audio levels independently

## Project Structure

```
MacAudio/
├── MacAudio/                    # Swift menu bar app
│   ├── MacAudioApp.swift        # App entry point
│   ├── AudioEngine/
│   │   ├── AudioConstants.swift       # Shared constants (sample rate, buffer size, etc.)
│   │   ├── AudioMixer.swift           # Orchestrates capture and writes to ring buffer
│   │   ├── MicCapture.swift           # Mic input via CoreAudio IOProc
│   │   ├── SystemAudioCapture.swift   # System audio via CATapDescription + aggregate device
│   │   └── SharedRingBufferWriter.swift  # Swift wrapper for the C ring buffer API
│   ├── Models/
│   │   ├── AppState.swift             # Observable state for the UI
│   │   └── AudioDeviceManager.swift   # Enumerates audio devices
│   ├── Views/
│   │   ├── MenuBarView.swift          # NSMenu-based menu bar UI
│   │   ├── VolumeSlider.swift         # Gain sliders
│   │   └── DevicePickerView.swift     # Mic device selector
│   └── Utilities/
│       ├── Permissions.swift          # Permission checks
│       └── DriverInstaller.swift      # In-app driver installation
├── MacAudioDriver/              # C AudioServerPlugin HAL driver
│   ├── MacAudioDriver.c         # Full AudioServerPlugInDriverInterface implementation
│   ├── SharedRingBuffer.c       # Lock-free SPSC ring buffer (POSIX shm)
│   └── SharedRingBuffer.h       # Public header (shared with Swift via bridging)
├── Installer/
│   ├── install.sh
│   └── uninstall.sh
└── project.yml                  # xcodegen project definition
```

## Key Constants

| Constant | Value |
|---|---|
| Sample rate | 48000 Hz (also supports 44100, 96000) |
| Channels | 2 (stereo interleaved Float32) |
| Ring buffer | 16384 frames |
| Shared memory | `/macaudio_ringbuffer` |
| Virtual device UID | `MacAudioDevice_UID` |

## Debugging

```bash
# Driver logs
log show --last 10s --predicate 'subsystem == "com.macaudio.driver"' --info --debug

# App logs
log show --last 10s --predicate 'subsystem == "com.macaudio.app"' --info --debug

# coreaudiod logs mentioning MacAudio
log show --last 10s --predicate 'process == "coreaudiod" AND message CONTAINS "MacAudio"'

# Check driver process
ps aux | grep MacAudioDriver
```

## macOS Tahoe (26.x) Notes

On macOS Tahoe, HAL plugins load out-of-process via `Core-Audio-Driver-Service.helper`. This has two implications:

- **Driver signing is mandatory.** Ad-hoc signed drivers are silently skipped. You must sign with a Developer ID certificate and remove the `com.apple.provenance` extended attribute after copying to `/Library/Audio/Plug-Ins/HAL/`.
- **New QueryInterface UUID.** Tahoe sends UUID `EEA5773D-CC43-49F1-8E00-8F96E7D23B17` in addition to the standard `EAAF5B97-B965-4F68-9815-E718330862D5`. The driver handles both.

## License

All rights reserved.
