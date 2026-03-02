#ifndef SharedRingBuffer_h
#define SharedRingBuffer_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define kSHM_Name           "/macaudio_ringbuffer"
#define kRingBufferFrames   16384u
#define kNumChannels        2u
#define kMaxFrameSize       (kRingBufferFrames * kNumChannels)
#define kSHM_BufferBytes    (kMaxFrameSize * sizeof(float))

// Opaque type — Swift interacts only through accessor functions
typedef struct SharedRingBuffer SharedRingBuffer;

// Lifecycle
SharedRingBuffer* SharedRingBuffer_CreateOrOpen(int forWriting);
void SharedRingBuffer_Close(SharedRingBuffer* rb);
void SharedRingBuffer_Destroy(void);

// Writer accessors (used by the Swift app)
void SharedRingBuffer_SetActive(SharedRingBuffer* rb, uint32_t active);
void SharedRingBuffer_SetSampleRate(SharedRingBuffer* rb, uint32_t rate);
uint64_t SharedRingBuffer_GetWriteHead(SharedRingBuffer* rb);
void SharedRingBuffer_Write(SharedRingBuffer* rb, const float* frames, uint32_t frameCount);

// Reader accessors (used by the AudioServerPlugin)
uint32_t SharedRingBuffer_GetActive(SharedRingBuffer* rb);
uint32_t SharedRingBuffer_GetSampleRate(SharedRingBuffer* rb);
uint64_t SharedRingBuffer_GetReadHead(SharedRingBuffer* rb);
uint64_t SharedRingBuffer_Read(SharedRingBuffer* rb, float* outBuffer, uint32_t frameCount);

// Size constant accessible from Swift
uint64_t SharedRingBuffer_GetSHMSize(void);

#ifdef __cplusplus
}
#endif

#endif
