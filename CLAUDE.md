# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MacAudio is a macOS menu bar app that captures microphone + system audio, mixes them, and exposes the mix as a virtual input device via a CoreAudio HAL AudioServerPlugin. Requires macOS 14.2+ (uses Core Audio Taps API).

## Build Commands

Project uses xcodegen to generate the Xcode project from `project.yml`.

### Regenerate Xcode project
```bash
xcodegen generate
```
**After every `xcodegen generate`, you MUST manually restore:**
1. `MacAudio/MacAudio.entitlements` — xcodegen wipes it to an empty dict. Restore `com.apple.security.device.audio-input` entitlement.
2. `MacAudioDriver/Info.plist` — xcodegen strips `CFBundleExecutable`. Re-add `<key>CFBundleExecutable</key><string>$(EXECUTABLE_NAME)</string>`.

### Build the driver (standalone)
```bash
xcodebuild -project MacAudio.xcodeproj -target MacAudioDriver -configuration Debug clean build \
  CODE_SIGN_IDENTITY="Developer ID Application" DEVELOPMENT_TEAM=RB4QV9W52C CODE_SIGN_STYLE=Manual
```

### Build the app (includes driver as dependency)
```bash
xcodebuild -project MacAudio.xcodeproj -target MacAudio -configuration Debug build \
  CODE_SIGN_IDENTITY="Developer ID Application" DEVELOPMENT_TEAM=RB4QV9W52C CODE_SIGN_STYLE=Manual
```

### Install driver to system
```bash
osascript -e 'do shell script "rm -rf /Library/Audio/Plug-Ins/HAL/MacAudioDriver.driver ; \
cp -R build/Debug/MacAudioDriver.driver /Library/Audio/Plug-Ins/HAL/ && \
xattr -rc /Library/Audio/Plug-Ins/HAL/MacAudioDriver.driver && \
killall coreaudiod" with administrator privileges'
```

### Verify driver is loaded
```bash
system_profiler SPAudioDataType | grep -A5 "MacAudio"
```

## Architecture

### Two-process design
The app and driver run in separate processes and communicate via POSIX shared memory (`/macaudio_ringbuffer`).

**Swift App (MacAudio target)** — Menu bar app that captures audio:
- `AudioMixer` orchestrates `MicCapture` (AVAudioEngine tap) and `SystemAudioCapture` (Core Audio Taps API via `CATapDescription` + private aggregate device), mixes them, and writes interleaved float samples to the shared ring buffer.
- `SharedRingBufferWriter` wraps the C `SharedRingBuffer` API via bridging header for writing from Swift.
- `AppState` (ObservableObject) manages all UI state and audio lifecycle. `AppDelegate` renders an NSMenu-based menu bar UI.

**C Driver (MacAudioDriver target)** — AudioServerPlugin HAL driver (`MacAudioDriver.c`, ~1100 lines):
- Implements `AudioServerPlugInDriverInterface` (COM-style vtable). Presents a single virtual input device with one stereo stream + volume/mute controls.
- Reads mixed audio from the shared ring buffer during `DoIOOperation`.
- Object hierarchy: PlugIn (ID 1) → Device (ID 2) → Stream (ID 3), Volume Control (ID 4), Mute Control (ID 5).

### Shared ring buffer (`SharedRingBuffer.c/.h`)
Lock-free SPSC ring buffer over POSIX shared memory. Opaque `SharedRingBuffer*` type for Swift interop. The same C source is compiled into both targets — the app opens with `forWriting=1`, the driver with `forWriting=0`.

### Data flow
```
Mic (AVAudioEngine) ──┐
                      ├─→ AudioMixer ──→ SharedRingBuffer (shm) ──→ HAL Driver ──→ Virtual Input Device
System (CATapDesc) ───┘
```

## macOS Tahoe Driver Signing

On macOS Tahoe (26.x), HAL plugins load out-of-process via `Core-Audio-Driver-Service.helper`. **Ad-hoc signed drivers are silently skipped.** Must sign with Developer ID and remove provenance xattr after install.

Tahoe also uses a new QueryInterface UUID `EEA5773D-CC43-49F1-8E00-8F96E7D23B17` — the driver must respond to this in addition to the standard `EAAF5B97-B965-4F68-9815-E718330862D5` and IUnknown UUIDs.

## Debugging

```bash
# Driver startup logs
/usr/bin/log show --last 10s --predicate 'subsystem == "com.macaudio.driver"' --info --debug

# coreaudiod logs about the driver
/usr/bin/log show --last 10s --predicate 'process == "coreaudiod" AND message CONTAINS "MacAudio"'

# Check driver process
ps aux | grep "MacAudioDriver"

# App logs
/usr/bin/log show --last 10s --predicate 'subsystem == "com.macaudio.app"'
```

## Key Constants

Audio format: 2-channel interleaved Float32 at 48000 Hz (also supports 44100, 96000). Ring buffer: 16384 frames. Shared memory name: `/macaudio_ringbuffer`. Virtual device UID: `MacAudioDevice_UID`.

## Permissions

The app requires:
- **Microphone access** (`NSMicrophoneUsageDescription` in Info.plist, `com.apple.security.device.audio-input` entitlement)
- **Screen & System Audio Recording** (for `AudioHardwareCreateProcessTap`)
- The app is `LSUIElement=true` (no dock icon, menu bar only)
