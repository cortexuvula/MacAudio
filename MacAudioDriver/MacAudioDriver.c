#include "MacAudioDriver.h"
#include "SharedRingBuffer.h"
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <os/log.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>
#include <time.h>

const Float64 kSupportedSampleRates[kNumSupportedSampleRates] = {44100.0, 48000.0, 96000.0};

#pragma mark - Forward Declarations

static HRESULT  MacAudio_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG    MacAudio_AddRef(void* inDriver);
static ULONG    MacAudio_Release(void* inDriver);
static OSStatus MacAudio_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus MacAudio_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
static OSStatus MacAudio_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus MacAudio_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus MacAudio_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus MacAudio_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static OSStatus MacAudio_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static Boolean  MacAudio_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus MacAudio_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus MacAudio_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus MacAudio_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus MacAudio_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData);
static OSStatus MacAudio_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus MacAudio_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus MacAudio_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
static OSStatus MacAudio_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outIsInput);
static OSStatus MacAudio_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus MacAudio_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
static OSStatus MacAudio_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

#pragma mark - Driver Interface

static AudioServerPlugInDriverInterface gDriverInterface = {
    NULL,
    MacAudio_QueryInterface,
    MacAudio_AddRef,
    MacAudio_Release,
    MacAudio_Initialize,
    MacAudio_CreateDevice,
    MacAudio_DestroyDevice,
    MacAudio_AddDeviceClient,
    MacAudio_RemoveDeviceClient,
    MacAudio_PerformDeviceConfigurationChange,
    MacAudio_AbortDeviceConfigurationChange,
    MacAudio_HasProperty,
    MacAudio_IsPropertySettable,
    MacAudio_GetPropertyDataSize,
    MacAudio_GetPropertyData,
    MacAudio_SetPropertyData,
    MacAudio_StartIO,
    MacAudio_StopIO,
    MacAudio_GetZeroTimeStamp,
    MacAudio_WillDoIOOperation,
    MacAudio_BeginIOOperation,
    MacAudio_DoIOOperation,
    MacAudio_EndIOOperation,
};

static AudioServerPlugInDriverInterface* gDriverInterfacePtr = &gDriverInterface;
static AudioServerPlugInDriverRef gDriverRef = &gDriverInterfacePtr;

#pragma mark - Global State

static AudioServerPlugInHostRef  gHost = NULL;
static _Atomic(SharedRingBuffer*) gRingBuffer = NULL;
// Mappings kept alive past their replacement (StopIO detach or a reconnection
// swap) so any DoIOOperation that already loaded the old gRingBuffer pointer
// can finish without dereferencing unmapped memory. munmap is deferred to the
// next StartIO under gDevice_IOMutex, by which time the HAL guarantees all IO
// cycles of the prior session have ended. Multiple mappings can pile up when
// the app quits/relaunches repeatedly while one recording client stays
// continuously connected (each replacement retires one mapping mid-IO), so
// this is a bounded ring of slots rather than a single pointer; a slot is
// reaped per StartIO quiet window. Guarded by gDevice_IOMutex.
#define kRingBuffer_PendingCloseSlots 8
static SharedRingBuffer*         gRingBuffer_PendingClose[kRingBuffer_PendingCloseSlots];
static UInt32                    gRingBuffer_PendingCloseCount = 0;
// R1 reconnection state. The realtime path caches the shm epoch it is reading
// through and compares it every IO cycle; a mismatch (the writer segment was
// unlinked and recreated — app quit→relaunch) only sets gRingBuffer_ReopenNeeded.
// The actual reopen (syscalls) happens on the non-realtime poller thread.
static _Atomic uint32_t          gRingBuffer_EpochCache = 0;
static _Atomic bool              gRingBuffer_ReopenNeeded = false;
static _Atomic bool              gRingBuffer_ReopenThreadStarted = false;
static _Atomic Float64           gDevice_SampleRate = kDevice_DefaultSampleRate;
static _Atomic UInt32            gDevice_IOClientCount = 0;
static _Atomic bool              gDevice_IOIsRunning = false;
static _Atomic double            gDevice_HostTicksPerFrame = 0.0;
static _Atomic uint64_t          gDevice_AnchorHostTime = 0;
static UInt64                    gDevice_TimeStampCounter = 0;
static _Atomic Float32           gVolume_Input_Master = 1.0f;
static _Atomic bool              gMute_Input_Master = false;
static pthread_mutex_t           gDevice_IOMutex = PTHREAD_MUTEX_INITIALIZER;
static os_log_t                  gLog = NULL;

static void recalcHostTicksPerFrame(void) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    Float64 nanosPerTick = (Float64)timebase.numer / (Float64)timebase.denom;
    Float64 sampleRate = atomic_load_explicit(&gDevice_SampleRate, memory_order_acquire);
    Float64 ticks = (1000000000.0 / sampleRate) / nanosPerTick;
    atomic_store_explicit(&gDevice_HostTicksPerFrame, ticks, memory_order_release);
}

#pragma mark - Entry Point

void* MacAudioDriver_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID) {
    (void)allocator;

    if (!gLog) gLog = os_log_create(kPlugIn_BundleID, "driver");
    os_log(gLog, "MacAudioDriver_Create called");

    CFUUIDRef audioPlugInTypeUUID = CFUUIDCreateFromString(NULL,
        CFSTR("443ABAB8-E7B3-491A-B985-BEB9187030DB"));
    if (!CFEqual(requestedTypeUUID, audioPlugInTypeUUID)) {
        os_log_error(gLog, "MacAudioDriver_Create: UUID mismatch, returning NULL");
        CFRelease(audioPlugInTypeUUID);
        return NULL;
    }
    CFRelease(audioPlugInTypeUUID);

    os_log(gLog, "MacAudioDriver_Create returning driver ref %p", gDriverRef);
    return gDriverRef;
}

#pragma mark - IUnknown

static HRESULT MacAudio_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    if (!gLog) gLog = os_log_create(kPlugIn_BundleID, "driver");
    CFUUIDRef interfaceUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    CFStringRef uuidStr = CFUUIDCreateString(NULL, interfaceUUID);
    os_log(gLog, "QueryInterface called for UUID: %{public}@", uuidStr);
    CFRelease(uuidStr);

    CFUUIDRef iUnknownUUID = CFUUIDGetConstantUUIDWithBytes(NULL,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
    CFUUIDRef plugInInterfaceUUID = CFUUIDCreateFromString(NULL,
        CFSTR("EAAF5B97-B965-4F68-9815-E718330862D5"));
    // macOS Tahoe out-of-process driver interface UUID
    CFUUIDRef plugInInterfaceUUID2 = CFUUIDCreateFromString(NULL,
        CFSTR("EEA5773D-CC43-49F1-8E00-8F96E7D23B17"));

    if (CFEqual(interfaceUUID, iUnknownUUID) || CFEqual(interfaceUUID, plugInInterfaceUUID) || CFEqual(interfaceUUID, plugInInterfaceUUID2)) {
        os_log(gLog, "QueryInterface: matched, returning driver ref");
        CFRelease(interfaceUUID);
        CFRelease(plugInInterfaceUUID);
        CFRelease(plugInInterfaceUUID2);
        MacAudio_AddRef(inDriver);
        *outInterface = gDriverRef;
        return S_OK;
    }

    os_log(gLog, "QueryInterface: no match, returning E_NOINTERFACE");
    CFRelease(interfaceUUID);
    CFRelease(plugInInterfaceUUID);
    CFRelease(plugInInterfaceUUID2);
    *outInterface = NULL;
    return E_NOINTERFACE;
}

static ULONG MacAudio_AddRef(void* inDriver) {
    (void)inDriver;
    return 1;
}

static ULONG MacAudio_Release(void* inDriver) {
    (void)inDriver;
    return 1;
}

#pragma mark - Initialization

static OSStatus MacAudio_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    (void)inDriver;
    gHost = inHost;
    gLog = os_log_create(kPlugIn_BundleID, "driver");
    os_log(gLog, "MacAudio_Initialize called, host=%p", inHost);

    recalcHostTicksPerFrame();
    os_log(gLog, "Timebase configured: ticksPerFrame=%f",
           atomic_load_explicit(&gDevice_HostTicksPerFrame, memory_order_acquire));

    // Don't open the ring buffer here — the app may not have created
    // the shm segment yet. We open it fresh in StartIO instead.
    atomic_store_explicit(&gRingBuffer, NULL, memory_order_release);

    os_log(gLog, "MacAudio_Initialize completed successfully");
    return noErr;
}

#pragma mark - Device Lifecycle

static OSStatus MacAudio_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID) {
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus MacAudio_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) {
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus MacAudio_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return noErr;
}

static OSStatus MacAudio_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return noErr;
}

static OSStatus MacAudio_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

static OSStatus MacAudio_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

#pragma mark - Property Support Helpers

static Boolean PlugIn_HasProperty(pid_t clientPID, const AudioObjectPropertyAddress* addr) {
    (void)clientPID;
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
    }
    return false;
}

static Boolean Device_HasProperty(pid_t clientPID, const AudioObjectPropertyAddress* addr) {
    (void)clientPID;
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIcon:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyStreamConfiguration:
        case kAudioDevicePropertyPreferredChannelLayout:
            return true;
    }
    return false;
}

static Boolean Stream_HasProperty(pid_t clientPID, const AudioObjectPropertyAddress* addr) {
    (void)clientPID;
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
    }
    return false;
}

static Boolean Volume_HasProperty(pid_t clientPID, const AudioObjectPropertyAddress* addr) {
    (void)clientPID;
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyDecibelRange:
            return true;
    }
    return false;
}

static Boolean Mute_HasProperty(pid_t clientPID, const AudioObjectPropertyAddress* addr) {
    (void)clientPID;
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioBooleanControlPropertyValue:
            return true;
    }
    return false;
}

#pragma mark - HasProperty

static Boolean MacAudio_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) {
    (void)inDriver;
    switch (inObjectID) {
        case kObjectID_PlugIn:              return PlugIn_HasProperty(inClientProcessID, inAddress);
        case kObjectID_Device:              return Device_HasProperty(inClientProcessID, inAddress);
        case kObjectID_Stream_Input:        return Stream_HasProperty(inClientProcessID, inAddress);
        case kObjectID_Volume_Input_Master: return Volume_HasProperty(inClientProcessID, inAddress);
        case kObjectID_Mute_Input_Master:   return Mute_HasProperty(inClientProcessID, inAddress);
    }
    return false;
}

#pragma mark - IsPropertySettable

static OSStatus MacAudio_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    (void)inDriver; (void)inClientProcessID;

    *outIsSettable = false;

    switch (inObjectID) {
        case kObjectID_Device:
            if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
                *outIsSettable = true;
            }
            break;
        case kObjectID_Stream_Input:
            if (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
                inAddress->mSelector == kAudioStreamPropertyPhysicalFormat) {
                *outIsSettable = true;
            }
            break;
        case kObjectID_Volume_Input_Master:
            if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue ||
                inAddress->mSelector == kAudioLevelControlPropertyDecibelValue) {
                *outIsSettable = true;
            }
            break;
        case kObjectID_Mute_Input_Master:
            if (inAddress->mSelector == kAudioBooleanControlPropertyValue) {
                *outIsSettable = true;
            }
            break;
    }

    return noErr;
}

#pragma mark - GetPropertyDataSize

static OSStatus MacAudio_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) {
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;

    switch (inObjectID) {
        case kObjectID_PlugIn:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    return noErr;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                case kAudioObjectPropertyManufacturer:
                    *outDataSize = sizeof(CFStringRef);
                    return noErr;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                case kAudioPlugInPropertyTranslateUIDToDevice:
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                case kAudioPlugInPropertyResourceBundle:
                    *outDataSize = sizeof(CFStringRef);
                    return noErr;
            }
            break;

        case kObjectID_Device:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    return noErr;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                    *outDataSize = sizeof(CFStringRef);
                    return noErr;
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyClockDomain:
                    *outDataSize = sizeof(UInt32);
                    return noErr;
                case kAudioDevicePropertyRelatedDevices:
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyIsHidden:
                    *outDataSize = sizeof(UInt32);
                    return noErr;
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    *outDataSize = sizeof(UInt32);
                    return noErr;
                case kAudioDevicePropertyStreams:
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                case kAudioObjectPropertyControlList:
                    *outDataSize = 2 * sizeof(AudioObjectID);
                    return noErr;
                case kAudioDevicePropertyNominalSampleRate:
                    *outDataSize = sizeof(Float64);
                    return noErr;
                case kAudioDevicePropertyAvailableNominalSampleRates:
                    *outDataSize = kNumSupportedSampleRates * sizeof(AudioValueRange);
                    return noErr;
                case kAudioDevicePropertyIcon:
                    *outDataSize = sizeof(CFURLRef);
                    return noErr;
                case kAudioObjectPropertyOwnedObjects:
                    *outDataSize = 3 * sizeof(AudioObjectID);
                    return noErr;
                case kAudioDevicePropertyStreamConfiguration: {
                    *outDataSize = (UInt32)(offsetof(AudioBufferList, mBuffers[1]));
                    return noErr;
                }
                case kAudioDevicePropertyPreferredChannelLayout:
                    *outDataSize = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions[kDevice_NumChannels]));
                    return noErr;
            }
            break;

        case kObjectID_Stream_Input:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    return noErr;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                    *outDataSize = sizeof(UInt32);
                    return noErr;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                    *outDataSize = sizeof(AudioStreamBasicDescription);
                    return noErr;
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    *outDataSize = kNumSupportedSampleRates * sizeof(AudioStreamRangedDescription);
                    return noErr;
            }
            break;

        case kObjectID_Volume_Input_Master:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    return noErr;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                case kAudioLevelControlPropertyScalarValue:
                    *outDataSize = sizeof(Float32);
                    return noErr;
                case kAudioLevelControlPropertyDecibelValue:
                    *outDataSize = sizeof(Float32);
                    return noErr;
                case kAudioLevelControlPropertyDecibelRange:
                    *outDataSize = sizeof(AudioValueRange);
                    return noErr;
            }
            break;

        case kObjectID_Mute_Input_Master:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    return noErr;
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                case kAudioBooleanControlPropertyValue:
                    *outDataSize = sizeof(UInt32);
                    return noErr;
            }
            break;
    }

    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - GetPropertyData

static inline Float32 ScalarToDecibels(Float32 scalar) {
    if (scalar <= 0.0f) return -96.0f;
    return 20.0f * log10f(scalar);
}

static inline Float32 DecibelsToScalar(Float32 dB) {
    if (dB <= -96.0f) return 0.0f;
    return powf(10.0f, dB / 20.0f);
}

static OSStatus MacAudio_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    (void)inDriver; (void)inClientProcessID;

    switch (inObjectID) {

    // ========== PLUGIN ==========
    case kObjectID_PlugIn:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioObjectClassID;
                return noErr;

            case kAudioObjectPropertyClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioPlugInClassID;
                return noErr;

            case kAudioObjectPropertyOwner:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioObjectID);
                *((AudioObjectID*)outData) = kAudioObjectUnknown;
                return noErr;

            case kAudioObjectPropertyManufacturer:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(CFStringRef);
                *((CFStringRef*)outData) = CFSTR(kDevice_Manufacturer);
                return noErr;

            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioObjectID);
                *((AudioObjectID*)outData) = kObjectID_Device;
                return noErr;

            case kAudioPlugInPropertyTranslateUIDToDevice: {
                if (!inQualifierData || inQualifierDataSize < sizeof(CFStringRef)) {
                    return kAudioHardwareBadPropertySizeError;
                }
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                CFStringRef uid = *((CFStringRef*)inQualifierData);
                *outDataSize = sizeof(AudioObjectID);
                if (CFStringCompare(uid, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo) {
                    *((AudioObjectID*)outData) = kObjectID_Device;
                } else {
                    *((AudioObjectID*)outData) = kAudioObjectUnknown;
                }
                return noErr;
            }

            case kAudioPlugInPropertyResourceBundle:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(CFStringRef);
                *((CFStringRef*)outData) = CFSTR("");
                return noErr;
        }
        break;

    // ========== DEVICE ==========
    case kObjectID_Device:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioObjectClassID;
                return noErr;

            case kAudioObjectPropertyClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioDeviceClassID;
                return noErr;

            case kAudioObjectPropertyOwner:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioObjectID);
                *((AudioObjectID*)outData) = kObjectID_PlugIn;
                return noErr;

            case kAudioObjectPropertyName:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(CFStringRef);
                *((CFStringRef*)outData) = CFSTR(kDevice_Name);
                return noErr;

            case kAudioObjectPropertyManufacturer:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(CFStringRef);
                *((CFStringRef*)outData) = CFSTR(kDevice_Manufacturer);
                return noErr;

            case kAudioDevicePropertyDeviceUID:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(CFStringRef);
                *((CFStringRef*)outData) = CFSTR(kDevice_UID);
                return noErr;

            case kAudioDevicePropertyModelUID:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(CFStringRef);
                *((CFStringRef*)outData) = CFSTR(kDevice_ModelUID);
                return noErr;

            case kAudioDevicePropertyTransportType:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = kAudioDeviceTransportTypeVirtual;
                return noErr;

            case kAudioDevicePropertyRelatedDevices:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioObjectID);
                *((AudioObjectID*)outData) = kObjectID_Device;
                return noErr;

            case kAudioDevicePropertyClockDomain:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 0;
                return noErr;

            case kAudioDevicePropertyDeviceIsAlive:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 1;
                return noErr;

            case kAudioDevicePropertyDeviceIsRunning:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = atomic_load_explicit(&gDevice_IOIsRunning, memory_order_acquire) ? 1 : 0;
                return noErr;

            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 1;
                return noErr;

            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 0;
                return noErr;

            case kAudioDevicePropertyIsHidden:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 0;
                return noErr;

            case kAudioDevicePropertyLatency:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 0;
                return noErr;

            case kAudioDevicePropertySafetyOffset:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 0;
                return noErr;

            case kAudioDevicePropertyZeroTimeStampPeriod:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = kDevice_RingBufferFrames;
                return noErr;

            case kAudioDevicePropertyStreams:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput ||
                    inAddress->mScope == kAudioObjectPropertyScopeGlobal) {
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *outDataSize = sizeof(AudioObjectID);
                    *((AudioObjectID*)outData) = kObjectID_Stream_Input;
                } else {
                    *outDataSize = 0;
                }
                return noErr;

            case kAudioObjectPropertyOwnedObjects:
                if (inDataSize < 3 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = 3 * sizeof(AudioObjectID);
                ((AudioObjectID*)outData)[0] = kObjectID_Stream_Input;
                ((AudioObjectID*)outData)[1] = kObjectID_Volume_Input_Master;
                ((AudioObjectID*)outData)[2] = kObjectID_Mute_Input_Master;
                return noErr;

            case kAudioObjectPropertyControlList:
                if (inDataSize < 2 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = 2 * sizeof(AudioObjectID);
                ((AudioObjectID*)outData)[0] = kObjectID_Volume_Input_Master;
                ((AudioObjectID*)outData)[1] = kObjectID_Mute_Input_Master;
                return noErr;

            case kAudioDevicePropertyNominalSampleRate:
                if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(Float64);
                *((Float64*)outData) = atomic_load_explicit(&gDevice_SampleRate, memory_order_acquire);
                return noErr;

            case kAudioDevicePropertyAvailableNominalSampleRates: {
                if (inDataSize < kNumSupportedSampleRates * sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                UInt32 count = kNumSupportedSampleRates;
                *outDataSize = count * sizeof(AudioValueRange);
                AudioValueRange* ranges = (AudioValueRange*)outData;
                for (UInt32 i = 0; i < count; i++) {
                    ranges[i].mMinimum = kSupportedSampleRates[i];
                    ranges[i].mMaximum = kSupportedSampleRates[i];
                }
                return noErr;
            }

            case kAudioDevicePropertyStreamConfiguration: {
                if (inDataSize < offsetof(AudioBufferList, mBuffers[1])) return kAudioHardwareBadPropertySizeError;
                AudioBufferList* bufferList = (AudioBufferList*)outData;
                *outDataSize = (UInt32)(offsetof(AudioBufferList, mBuffers[1]));
                bufferList->mNumberBuffers = 1;
                bufferList->mBuffers[0].mNumberChannels = kDevice_NumChannels;
                bufferList->mBuffers[0].mDataByteSize = 0;
                bufferList->mBuffers[0].mData = NULL;
                return noErr;
            }

            case kAudioDevicePropertyPreferredChannelLayout: {
                if (inDataSize < offsetof(AudioChannelLayout, mChannelDescriptions[kDevice_NumChannels])) return kAudioHardwareBadPropertySizeError;
                AudioChannelLayout* layout = (AudioChannelLayout*)outData;
                *outDataSize = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions[kDevice_NumChannels]));
                layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                layout->mChannelBitmap = 0;
                layout->mNumberChannelDescriptions = kDevice_NumChannels;
                layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                layout->mChannelDescriptions[0].mChannelFlags = 0;
                layout->mChannelDescriptions[0].mCoordinates[0] = 0;
                layout->mChannelDescriptions[0].mCoordinates[1] = 0;
                layout->mChannelDescriptions[0].mCoordinates[2] = 0;
                layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
                layout->mChannelDescriptions[1].mChannelFlags = 0;
                layout->mChannelDescriptions[1].mCoordinates[0] = 0;
                layout->mChannelDescriptions[1].mCoordinates[1] = 0;
                layout->mChannelDescriptions[1].mCoordinates[2] = 0;
                return noErr;
            }

            case kAudioDevicePropertyIcon:
                if (inDataSize < sizeof(CFURLRef)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(CFURLRef);
                *((CFURLRef*)outData) = NULL;
                return noErr;
        }
        break;

    // ========== STREAM ==========
    case kObjectID_Stream_Input:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioObjectClassID;
                return noErr;

            case kAudioObjectPropertyClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioStreamClassID;
                return noErr;

            case kAudioObjectPropertyOwner:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioObjectID);
                *((AudioObjectID*)outData) = kObjectID_Device;
                return noErr;

            case kAudioStreamPropertyIsActive:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 1;
                return noErr;

            case kAudioStreamPropertyDirection:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 1; // 1 = input (from client perspective)
                return noErr;

            case kAudioStreamPropertyTerminalType:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = kAudioStreamTerminalTypeMicrophone;
                return noErr;

            case kAudioStreamPropertyStartingChannel:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 1;
                return noErr;

            case kAudioStreamPropertyLatency:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = 0;
                return noErr;

            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioStreamBasicDescription);
                AudioStreamBasicDescription* desc = (AudioStreamBasicDescription*)outData;
                desc->mSampleRate = atomic_load_explicit(&gDevice_SampleRate, memory_order_acquire);
                desc->mFormatID = kAudioFormatLinearPCM;
                desc->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
                desc->mBytesPerPacket = kDevice_BytesPerFrame;
                desc->mFramesPerPacket = 1;
                desc->mBytesPerFrame = kDevice_BytesPerFrame;
                desc->mChannelsPerFrame = kDevice_NumChannels;
                desc->mBitsPerChannel = kDevice_BitsPerChannel;
                return noErr;
            }

            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                if (inDataSize < kNumSupportedSampleRates * sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = kNumSupportedSampleRates * sizeof(AudioStreamRangedDescription);
                AudioStreamRangedDescription* descs = (AudioStreamRangedDescription*)outData;
                for (UInt32 i = 0; i < kNumSupportedSampleRates; i++) {
                    descs[i].mFormat.mSampleRate = kSupportedSampleRates[i];
                    descs[i].mFormat.mFormatID = kAudioFormatLinearPCM;
                    descs[i].mFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
                    descs[i].mFormat.mBytesPerPacket = kDevice_BytesPerFrame;
                    descs[i].mFormat.mFramesPerPacket = 1;
                    descs[i].mFormat.mBytesPerFrame = kDevice_BytesPerFrame;
                    descs[i].mFormat.mChannelsPerFrame = kDevice_NumChannels;
                    descs[i].mFormat.mBitsPerChannel = kDevice_BitsPerChannel;
                    descs[i].mSampleRateRange.mMinimum = kSupportedSampleRates[i];
                    descs[i].mSampleRateRange.mMaximum = kSupportedSampleRates[i];
                }
                return noErr;
            }
        }
        break;

    // ========== VOLUME CONTROL ==========
    case kObjectID_Volume_Input_Master:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioControlClassID;
                return noErr;

            case kAudioObjectPropertyClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioLevelControlClassID;
                return noErr;

            case kAudioObjectPropertyOwner:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioObjectID);
                *((AudioObjectID*)outData) = kObjectID_Device;
                return noErr;

            case kAudioLevelControlPropertyScalarValue:
                if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(Float32);
                *((Float32*)outData) = atomic_load_explicit(&gVolume_Input_Master, memory_order_relaxed);
                return noErr;

            case kAudioLevelControlPropertyDecibelValue:
                if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(Float32);
                *((Float32*)outData) = ScalarToDecibels(atomic_load_explicit(&gVolume_Input_Master, memory_order_relaxed));
                return noErr;

            case kAudioLevelControlPropertyDecibelRange: {
                if (inDataSize < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioValueRange);
                AudioValueRange* range = (AudioValueRange*)outData;
                range->mMinimum = -96.0;
                range->mMaximum = 0.0;
                return noErr;
            }
        }
        break;

    // ========== MUTE CONTROL ==========
    case kObjectID_Mute_Input_Master:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioControlClassID;
                return noErr;

            case kAudioObjectPropertyClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioClassID);
                *((AudioClassID*)outData) = kAudioBooleanControlClassID;
                return noErr;

            case kAudioObjectPropertyOwner:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(AudioObjectID);
                *((AudioObjectID*)outData) = kObjectID_Device;
                return noErr;

            case kAudioBooleanControlPropertyValue:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *outDataSize = sizeof(UInt32);
                *((UInt32*)outData) = atomic_load_explicit(&gMute_Input_Master, memory_order_relaxed) ? 1 : 0;
                return noErr;
        }
        break;
    }

    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - SetPropertyData

static OSStatus MacAudio_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData) {
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;

    switch (inObjectID) {
        case kObjectID_Device:
            if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
                if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
                Float64 newRate = *((const Float64*)inData);
                bool valid = false;
                for (UInt32 i = 0; i < kNumSupportedSampleRates; i++) {
                    if (kSupportedSampleRates[i] == newRate) {
                        valid = true;
                        break;
                    }
                }
                if (!valid) return kAudioHardwareIllegalOperationError;

                pthread_mutex_lock(&gDevice_IOMutex);
                atomic_store_explicit(&gDevice_SampleRate, newRate, memory_order_release);
                recalcHostTicksPerFrame();
                pthread_mutex_unlock(&gDevice_IOMutex);

                // Notify clients
                AudioObjectPropertyAddress changedAddresses[2];
                changedAddresses[0] = (AudioObjectPropertyAddress){
                    kAudioDevicePropertyNominalSampleRate,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                changedAddresses[1] = (AudioObjectPropertyAddress){
                    kAudioStreamPropertyPhysicalFormat,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, 0, NULL);
                gHost->PropertiesChanged(gHost, kObjectID_Device, 2, changedAddresses);
                return noErr;
            }
            break;

        case kObjectID_Stream_Input:
            if (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
                inAddress->mSelector == kAudioStreamPropertyPhysicalFormat) {
                if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                const AudioStreamBasicDescription* desc = (const AudioStreamBasicDescription*)inData;
                // Validate the full ASBD, not just the sample rate. The getter
                // advertises one specific format (see the matching getter), so
                // silently accepting e.g. an integer-PCM or mono request would
                // make the device report a format it doesn't actually render.
                bool rateValid = false;
                for (UInt32 i = 0; i < kNumSupportedSampleRates; i++) {
                    if (kSupportedSampleRates[i] == desc->mSampleRate) {
                        rateValid = true;
                        break;
                    }
                }
                if (!rateValid) return kAudioDeviceUnsupportedFormatError;
                if (desc->mFormatID != kAudioFormatLinearPCM) return kAudioDeviceUnsupportedFormatError;
                // Require float + native-endian + packed, but tolerate extra
                // benign flags the HAL may add (e.g. kAudioFormatFlagIsNonMixable).
                const UInt32 kRequiredFormatFlags =
                    kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
                if ((desc->mFormatFlags & kRequiredFormatFlags) != kRequiredFormatFlags)
                    return kAudioDeviceUnsupportedFormatError;
                if (desc->mChannelsPerFrame != kDevice_NumChannels) return kAudioDeviceUnsupportedFormatError;
                if (desc->mBitsPerChannel != kDevice_BitsPerChannel) return kAudioDeviceUnsupportedFormatError;
                if (desc->mBytesPerFrame != kDevice_BytesPerFrame) return kAudioDeviceUnsupportedFormatError;
                if (desc->mBytesPerPacket != kDevice_BytesPerFrame) return kAudioDeviceUnsupportedFormatError;
                if (desc->mFramesPerPacket != 1) return kAudioDeviceUnsupportedFormatError;

                pthread_mutex_lock(&gDevice_IOMutex);
                atomic_store_explicit(&gDevice_SampleRate, desc->mSampleRate, memory_order_release);
                recalcHostTicksPerFrame();
                pthread_mutex_unlock(&gDevice_IOMutex);

                AudioObjectPropertyAddress changedAddresses[2];
                changedAddresses[0] = (AudioObjectPropertyAddress){
                    kAudioDevicePropertyNominalSampleRate,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                changedAddresses[1] = (AudioObjectPropertyAddress){
                    kAudioStreamPropertyPhysicalFormat,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, 0, NULL);
                gHost->PropertiesChanged(gHost, kObjectID_Device, 2, changedAddresses);
                return noErr;
            }
            break;

        case kObjectID_Volume_Input_Master:
            if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue) {
                if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                Float32 newValue = *((const Float32*)inData);
                if (newValue < 0.0f) newValue = 0.0f;
                if (newValue > 1.0f) newValue = 1.0f;
                atomic_store_explicit(&gVolume_Input_Master, newValue, memory_order_relaxed);
                return noErr;
            }
            if (inAddress->mSelector == kAudioLevelControlPropertyDecibelValue) {
                if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                Float32 dB = *((const Float32*)inData);
                Float32 scalar = DecibelsToScalar(dB);
                if (scalar < 0.0f) scalar = 0.0f;
                if (scalar > 1.0f) scalar = 1.0f;
                atomic_store_explicit(&gVolume_Input_Master, scalar, memory_order_relaxed);
                return noErr;
            }
            break;

        case kObjectID_Mute_Input_Master:
            if (inAddress->mSelector == kAudioBooleanControlPropertyValue) {
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                UInt32 newValue = *((const UInt32*)inData);
                atomic_store_explicit(&gMute_Input_Master, (newValue != 0), memory_order_relaxed);
                return noErr;
            }
            break;
    }

    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - Ring Buffer Reconnection (R1)

// Deferred munmap of a replaced mapping. NEVER close here: when IO is running
// there is no quiet window, and a DoIOOperation that already loaded the old
// pointer may still be dereferencing it — munmap now would risk SIGBUS in
// coreaudiod. Park the mapping in a pending-close slot; the next StartIO
// reaps it once the HAL guarantees no in-flight IO cycles. Caller holds
// gDevice_IOMutex.
static void retire_ring_buffer_mapping(SharedRingBuffer* rb) {
    if (!rb) return;
    if (gRingBuffer_PendingCloseCount < kRingBuffer_PendingCloseSlots) {
        gRingBuffer_PendingClose[gRingBuffer_PendingCloseCount++] = rb;
        return;
    }
    // Fail safe: keep the mapping (reclaimed when coreaudiod eventually
    // restarts) rather than risk munmap'ing under a live reader.
    os_log_error(gLog, "Pending-close slots full; leaking ring buffer mapping %p", rb);
}

// munmap every retired mapping. Called only from StartIO's zero-client
// transition, where the HAL guarantees no IO cycles are in flight.
static void reap_pending_close_ring_buffers(void) {
    for (UInt32 i = 0; i < gRingBuffer_PendingCloseCount; i++) {
        SharedRingBuffer_Close(gRingBuffer_PendingClose[i]);
    }
    gRingBuffer_PendingCloseCount = 0;
}

// Reattach the shm segment after the realtime path flagged a replaced writer
// (epoch change) or when no mapping is attached (the initial StartIO open
// failed because the app hadn't created the segment yet). All syscalls
// (shm_open/mmap) live here on the poller thread, never on the realtime
// thread. Caller holds gDevice_IOMutex.
static void driver_reopen_ring_buffer_locked(void) {
    if (!atomic_load_explicit(&gDevice_IOIsRunning, memory_order_acquire)) {
        // IO stopped between the poller's unlocked pre-filter and acquiring
        // the mutex; StartIO/StopIO own the mapping lifecycle in that state.
        return;
    }
    bool flagged = atomic_load_explicit(&gRingBuffer_ReopenNeeded, memory_order_acquire);
    SharedRingBuffer* current = atomic_load_explicit(&gRingBuffer, memory_order_acquire);
    if (!flagged && current) {
        return;  // healthy — nothing to reopen
    }

    SharedRingBuffer* newRb = SharedRingBuffer_CreateOrOpen(0);
    if (!newRb) {
        // Segment doesn't exist yet — the app hasn't (re)created it. Leave
        // any existing mapping attached (realtime keeps outputting silence
        // for a stale epoch) and retry on a later poll.
        return;
    }
    // Recache BEFORE publishing the pointer: the release store here plus the
    // acquire of the pointer publish below means a realtime cycle that
    // observes the new mapping also observes the matching epoch cache, so it
    // doesn't flag the fresh segment as stale.
    atomic_store_explicit(&gRingBuffer_EpochCache,
                          SharedRingBuffer_GetEpoch(newRb), memory_order_release);
    SharedRingBuffer* oldRb = atomic_exchange_explicit(&gRingBuffer, newRb, memory_order_acq_rel);
    retire_ring_buffer_mapping(oldRb);
    atomic_store_explicit(&gRingBuffer_ReopenNeeded, false, memory_order_release);
    os_log(gLog, "Reopen: reattached ring buffer (epoch %u)",
           SharedRingBuffer_GetEpoch(newRb));
}

// The realtime path can only set a flag; this thread does the reconnect work.
// It polls: a realtime callback must not signal a condvar (lock + syscall),
// so wakeups are time-based instead. The unlocked pre-filter is two atomic
// loads; the thread does nothing while no client is running (StartIO/StopIO
// fully own the mapping then) or while the mapping is healthy.
static void* ring_buffer_reopen_thread_entry(void* arg) {
    (void)arg;
    for (;;) {
        struct timespec nap = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&nap, NULL);

        if (!atomic_load_explicit(&gDevice_IOIsRunning, memory_order_acquire)) {
            continue;
        }
        if (!atomic_load_explicit(&gRingBuffer_ReopenNeeded, memory_order_acquire) &&
            atomic_load_explicit(&gRingBuffer, memory_order_acquire) != NULL) {
            continue;
        }

        pthread_mutex_lock(&gDevice_IOMutex);
        driver_reopen_ring_buffer_locked();
        pthread_mutex_unlock(&gDevice_IOMutex);
    }
    return NULL;  // unreachable; the thread lives as long as coreaudiod
}

// Start the poller exactly once, lazily from StartIO (never the realtime
// path). Detached: the plugin is never unloaded within a coreaudiod process
// lifetime, so the thread simply runs until the process exits. Caller holds
// gDevice_IOMutex.
static void ensure_ring_buffer_reopen_thread(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&gRingBuffer_ReopenThreadStarted,
                                                 &expected, true,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        return;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int rc = pthread_create(&thread, &attr, ring_buffer_reopen_thread_entry, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        // Allow a retry on a later StartIO rather than disabling
        // reconnection for the process. Until a retry succeeds, behavior is
        // the pre-R1 one: a replaced segment means silence until the client
        // stops/restarts the device.
        atomic_store_explicit(&gRingBuffer_ReopenThreadStarted, false, memory_order_release);
        os_log_error(gLog, "Failed to start ring buffer reopen thread (%d)", rc);
    }
}

#pragma mark - IO Operations

static OSStatus MacAudio_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    pthread_mutex_lock(&gDevice_IOMutex);

    UInt32 clientCount = atomic_load_explicit(&gDevice_IOClientCount, memory_order_acquire);
    if (clientCount == 0) {
        atomic_store_explicit(&gDevice_IOIsRunning, true, memory_order_release);
        atomic_store_explicit(&gDevice_AnchorHostTime, mach_absolute_time(), memory_order_release);
        gDevice_TimeStampCounter = 0;

        // Reap the deferred closes from the previous StopIO (and any mappings
        // retired mid-IO by the reconnection poller). Safe here because
        // we're under gDevice_IOMutex and the prior IO cycles have ended (HAL
        // guarantees this between StopIO and StartIO).
        reap_pending_close_ring_buffers();

        // Re-open the shm segment for this fresh IO cycle.
        SharedRingBuffer* oldRb = atomic_load_explicit(&gRingBuffer, memory_order_acquire);
        if (oldRb) {
            SharedRingBuffer_Close(oldRb);
        }
        SharedRingBuffer* newRb = SharedRingBuffer_CreateOrOpen(0);
        atomic_store_explicit(&gRingBuffer, newRb, memory_order_release);
        if (!newRb) {
            os_log_error(gLog, "StartIO: failed to open ring buffer");
            // Still continue - device can work, just silent. The reopen
            // poller retries while a client stays connected, so audio
            // recovers once the app creates the segment.
        } else {
            atomic_store_explicit(&gRingBuffer_EpochCache,
                                  SharedRingBuffer_GetEpoch(newRb), memory_order_release);
            atomic_store_explicit(&gRingBuffer_ReopenNeeded, false, memory_order_release);
        }

        // Lazily start the reconnection poller (no-op after the first client).
        ensure_ring_buffer_reopen_thread();
    }
    clientCount++;
    atomic_store_explicit(&gDevice_IOClientCount, clientCount, memory_order_release);

    pthread_mutex_unlock(&gDevice_IOMutex);

    // Log the locally-captured count, not the shared atomic, so the value is
    // exact for this call regardless of concurrent Start/Stop on other threads.
    os_log(gLog, "StartIO: client count = %u", clientCount);
    return noErr;
}

static OSStatus MacAudio_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    pthread_mutex_lock(&gDevice_IOMutex);

    UInt32 clientCount = atomic_load_explicit(&gDevice_IOClientCount, memory_order_acquire);
    if (clientCount > 0) {
        clientCount--;
    }
    if (clientCount == 0) {
        atomic_store_explicit(&gDevice_IOIsRunning, false, memory_order_release);
        // Detach the buffer atomically, but defer the actual munmap to the
        // next StartIO. Any DoIOOperation racing with this StopIO may have
        // already loaded gRingBuffer; munmap'ing now would risk SIGBUS in
        // coreaudiod. The HAL guarantees no new IO cycles will fire on this
        // device until the next StartIO, where it's safe to finish the close.
        SharedRingBuffer* oldRb = atomic_exchange_explicit(&gRingBuffer, NULL, memory_order_acq_rel);
        if (oldRb) {
            retire_ring_buffer_mapping(oldRb);
        }
    }
    atomic_store_explicit(&gDevice_IOClientCount, clientCount, memory_order_release);

    pthread_mutex_unlock(&gDevice_IOMutex);

    os_log(gLog, "StopIO: client count = %u", clientCount);
    return noErr;
}

static OSStatus MacAudio_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    UInt64 anchorHostTime = atomic_load_explicit(&gDevice_AnchorHostTime, memory_order_acquire);
    Float64 hostTicksPerFrame = atomic_load_explicit(&gDevice_HostTicksPerFrame, memory_order_acquire);

    // Guard against an uninitialized/zero hostTicksPerFrame (e.g. called before
    // MacAudio_Initialize finishes, or if the sample rate were ever zero). A
    // division here would yield inf/NaN and the casts to UInt64 would be UB.
    if (hostTicksPerFrame <= 0.0) {
        *outSampleTime = 0;
        *outHostTime = anchorHostTime;
        *outSeed = 1;
        return noErr;
    }

    UInt64 currentHostTime = mach_absolute_time();
    Float64 elapsedTicks = (Float64)(currentHostTime - anchorHostTime);
    Float64 elapsedFrames = elapsedTicks / hostTicksPerFrame;
    UInt64 cycleCount = (UInt64)(elapsedFrames / (Float64)kDevice_RingBufferFrames);

    *outSampleTime = (Float64)(cycleCount * kDevice_RingBufferFrames);
    *outHostTime = anchorHostTime +
        (UInt64)((Float64)(cycleCount * kDevice_RingBufferFrames) * hostTicksPerFrame);
    *outSeed = cycleCount + 1;

    return noErr;
}

static OSStatus MacAudio_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outIsInput) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    *outWillDo = false;
    *outIsInput = true;

    if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        *outWillDo = true;
        *outIsInput = true;
    }

    return noErr;
}

static OSStatus MacAudio_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}

static OSStatus MacAudio_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
    (void)inDriver; (void)inDeviceObjectID; (void)inStreamObjectID;
    (void)inClientID; (void)inIOCycleInfo; (void)ioSecondaryBuffer;

    if (inOperationID != kAudioServerPlugInIOOperationReadInput) {
        return noErr;
    }

    Float32* outBuffer = (Float32*)ioMainBuffer;
    UInt32 totalSamples = inIOBufferFrameSize * kDevice_NumChannels;

    SharedRingBuffer* rb = atomic_load_explicit(&gRingBuffer, memory_order_acquire);

    if (!rb) {
        memset(outBuffer, 0, totalSamples * sizeof(Float32));
        return noErr;
    }

    // R1 reconnection: detect a REPLACED writer segment (the app quit and
    // relaunch unlinks + recreates the shm; teardown's final ownership
    // acquire bumps the epoch on the old inode before the unlink, so this
    // mapping observes it). Realtime does only two atomic loads and a
    // compare — no syscalls, no allocation — and flags the non-realtime
    // poller to reopen. This cycle outputs silence; audio resumes after the
    // poller reattaches the new segment.
    if (SharedRingBuffer_GetEpoch(rb) !=
        atomic_load_explicit(&gRingBuffer_EpochCache, memory_order_acquire)) {
        atomic_store_explicit(&gRingBuffer_ReopenNeeded, true, memory_order_release);
        memset(outBuffer, 0, totalSamples * sizeof(Float32));
        return noErr;
    }

    if (!SharedRingBuffer_GetActive(rb)) {
        memset(outBuffer, 0, totalSamples * sizeof(Float32));
        return noErr;
    }

    // Read from shared ring buffer using accessor function
    uint64_t framesToRead = SharedRingBuffer_Read(rb, outBuffer, inIOBufferFrameSize);

    // Zero-fill any remaining frames if underrun
    if (framesToRead < inIOBufferFrameSize) {
        memset(&outBuffer[framesToRead * kDevice_NumChannels], 0,
               (inIOBufferFrameSize - (UInt32)framesToRead) * kDevice_NumChannels * sizeof(Float32));
    }

    // Apply volume and mute
    bool muted = atomic_load_explicit(&gMute_Input_Master, memory_order_relaxed);
    Float32 vol = atomic_load_explicit(&gVolume_Input_Master, memory_order_relaxed);

    if (muted) {
        memset(outBuffer, 0, totalSamples * sizeof(Float32));
    } else if (vol < 1.0f) {
        for (UInt32 i = 0; i < totalSamples; i++) {
            outBuffer[i] *= vol;
        }
    }

    return noErr;
}

static OSStatus MacAudio_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}
