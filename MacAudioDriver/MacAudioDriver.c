#include "MacAudioDriver.h"
#include "SharedRingBuffer.h"
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <os/log.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>

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
static Float64                   gDevice_SampleRate = kDevice_DefaultSampleRate;
static UInt32                    gDevice_IOClientCount = 0;
static bool                      gDevice_IOIsRunning = false;
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
    Float64 ticks = (1000000000.0 / gDevice_SampleRate) / nanosPerTick;
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
                *((UInt32*)outData) = gDevice_IOIsRunning ? 1 : 0;
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
                *((Float64*)outData) = gDevice_SampleRate;
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
                desc->mSampleRate = gDevice_SampleRate;
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
                gDevice_SampleRate = newRate;
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
                bool valid = false;
                for (UInt32 i = 0; i < kNumSupportedSampleRates; i++) {
                    if (kSupportedSampleRates[i] == desc->mSampleRate) {
                        valid = true;
                        break;
                    }
                }
                if (!valid) return kAudioDeviceUnsupportedFormatError;

                pthread_mutex_lock(&gDevice_IOMutex);
                gDevice_SampleRate = desc->mSampleRate;
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

#pragma mark - IO Operations

static OSStatus MacAudio_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    pthread_mutex_lock(&gDevice_IOMutex);

    if (gDevice_IOClientCount == 0) {
        gDevice_IOIsRunning = true;
        atomic_store_explicit(&gDevice_AnchorHostTime, mach_absolute_time(), memory_order_release);
        gDevice_TimeStampCounter = 0;

        // Always re-open to pick up the latest shm segment
        SharedRingBuffer* oldRb = atomic_load_explicit(&gRingBuffer, memory_order_acquire);
        if (oldRb) {
            SharedRingBuffer_Close(oldRb);
        }
        SharedRingBuffer* newRb = SharedRingBuffer_CreateOrOpen(0);
        atomic_store_explicit(&gRingBuffer, newRb, memory_order_release);
        if (!newRb) {
            os_log_error(gLog, "StartIO: failed to open ring buffer");
            // Still continue - device can work, just silent
        }
    }
    gDevice_IOClientCount++;

    pthread_mutex_unlock(&gDevice_IOMutex);

    os_log(gLog, "StartIO: client count = %u", gDevice_IOClientCount);
    return noErr;
}

static OSStatus MacAudio_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    pthread_mutex_lock(&gDevice_IOMutex);

    if (gDevice_IOClientCount > 0) {
        gDevice_IOClientCount--;
    }
    if (gDevice_IOClientCount == 0) {
        gDevice_IOIsRunning = false;
        SharedRingBuffer* oldRb = atomic_load_explicit(&gRingBuffer, memory_order_acquire);
        if (oldRb) {
            SharedRingBuffer_Close(oldRb);
            atomic_store_explicit(&gRingBuffer, NULL, memory_order_release);
        }
    }

    pthread_mutex_unlock(&gDevice_IOMutex);

    os_log(gLog, "StopIO: client count = %u", gDevice_IOClientCount);
    return noErr;
}

static OSStatus MacAudio_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    UInt64 anchorHostTime = atomic_load_explicit(&gDevice_AnchorHostTime, memory_order_acquire);
    Float64 hostTicksPerFrame = atomic_load_explicit(&gDevice_HostTicksPerFrame, memory_order_acquire);

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

    if (!rb || !SharedRingBuffer_GetActive(rb)) {
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
