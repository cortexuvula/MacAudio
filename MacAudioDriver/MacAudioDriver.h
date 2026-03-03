#ifndef MacAudioDriver_h
#define MacAudioDriver_h

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>

#define kPlugIn_BundleID        "com.macaudio.driver"
#define kDevice_Name            "MacAudio Virtual Device"
#define kDevice_Manufacturer    "MacAudio"
#define kDevice_UID             "MacAudioDevice_UID"
#define kDevice_ModelUID        "MacAudioDevice_ModelUID"

enum {
    kObjectID_PlugIn                = kAudioObjectPlugInObject,
    kObjectID_Device                = 2,
    kObjectID_Stream_Input          = 3,
    kObjectID_Volume_Input_Master   = 4,
    kObjectID_Mute_Input_Master     = 5,
};

#define kDevice_NumChannels         2
#define kDevice_BitsPerChannel      32
#define kDevice_BytesPerFrame       (kDevice_NumChannels * (kDevice_BitsPerChannel / 8))
#define kDevice_DefaultSampleRate   48000.0
#define kDevice_RingBufferFrames    16384

#define kNumSupportedSampleRates    3
extern const Float64 kSupportedSampleRates[kNumSupportedSampleRates];

void* MacAudioDriver_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);

#endif
