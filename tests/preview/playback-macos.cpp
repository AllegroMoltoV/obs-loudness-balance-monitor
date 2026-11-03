// Independently authored finite PCM probe. API/source references: README.md.
#include "playback.h"
#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
using Clock = std::chrono::steady_clock;
struct Queue {
    AudioQueueRef value = nullptr;
    const std::vector<float>& pcm;
    size_t submitted = 0;
    OSStatus error = noErr;
    bool accepting = true;
    explicit Queue(const std::vector<float>& input) : pcm(input) {}
    ~Queue() { if (value) AudioQueueDispose(value, true); }
};
static bool device_matches(AudioDeviceID id, CFStringRef wanted)
{
    AudioObjectPropertyAddress addr{kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef uid = nullptr;
    UInt32 bytes = sizeof(uid);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &bytes, &uid) != noErr || !uid) return false;
    const bool match = CFEqual(uid, wanted);
    CFRelease(uid);
    if (!match) return false;
    addr.mSelector = kAudioDevicePropertyDeviceIsAlive;
    UInt32 alive = 0;
    bytes = sizeof(alive);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &bytes, &alive) != noErr || !alive) return false;
    addr.mSelector = kAudioDevicePropertyStreams;
    addr.mScope = kAudioDevicePropertyScopeOutput;
    bytes = 0;
    return AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &bytes) == noErr && bytes > 0;
}
static AudioDeviceID find_device(CFStringRef wanted)
{
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &bytes) != noErr || bytes == 0) return kAudioObjectUnknown;
    std::vector<AudioDeviceID> devices(bytes / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &bytes, devices.data()) != noErr) return kAudioObjectUnknown;
    for (size_t i = 0; i < bytes / sizeof(AudioDeviceID); ++i)
        if (device_matches(devices[i], wanted)) return devices[i];
    return kAudioObjectUnknown;
}
static void refill(void* data, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    auto& state = *static_cast<Queue*>(data);
    if (!state.accepting || state.error != noErr || interrupted) return;
    const size_t total = state.pcm.size() * sizeof(float);
    const size_t bytes = std::min(total - state.submitted, static_cast<size_t>(buffer->mAudioDataBytesCapacity));
    if (bytes == 0) return;
    std::memcpy(buffer->mAudioData, reinterpret_cast<const uint8_t*>(state.pcm.data()) + state.submitted, bytes);
    buffer->mAudioDataByteSize = static_cast<UInt32>(bytes);
    state.error = AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    if (state.error == noErr) state.submitted += bytes;
}
int playback(const std::vector<float>& pcm, uint32_t rate, const std::string& device, int cancel_ms)
{
    CFStringRef uid = CFStringCreateWithCString(nullptr, device.c_str(), kCFStringEncodingUTF8);
    if (!uid) return 2;
    const AudioDeviceID id = find_device(uid);
    if (id == kAudioObjectUnknown) { CFRelease(uid); std::cerr << "explicit output UID unavailable or enumeration failed\n"; return 2; }
    Queue state(pcm);
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 8;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 8;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    // All callbacks run on this thread's run loop. State is not shared with a worker.
    state.error = AudioQueueNewOutput(&format, refill, &state, CFRunLoopGetCurrent(), kCFRunLoopCommonModes, 0, &state.value);
    if (state.error == noErr) state.error = AudioQueueSetProperty(state.value, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
    if (state.error == noErr) state.error = AudioQueueSetParameter(state.value, kAudioQueueParam_Volume, 1.0f);
    for (int i = 0; i < 3 && state.error == noErr; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        state.error = AudioQueueAllocateBuffer(state.value, 4096 * 8, &buffer);
        if (state.error == noErr) refill(&state, state.value, buffer);
    }
    int result = 2;
    bool draining = false;
    const auto begin = Clock::now();
    if (state.error == noErr && !interrupted && cancel_ms != 0) {
        state.error = AudioQueueStart(state.value, nullptr);
        while (state.error == noErr && Clock::now() - begin < std::chrono::seconds(20)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
            if (interrupted || (cancel_ms >= 0 && elapsed >= cancel_ms)) { result = 3; break; }
            if (!device_matches(id, uid)) { std::cerr << "output device disappeared or changed\n"; break; }
            if (!draining && state.submitted == pcm.size() * sizeof(float)) {
                state.accepting = false;
                state.error = AudioQueueStop(state.value, false);
                if (state.error != noErr) break;
                draining = true;
            }
            if (draining) {
                UInt32 running = 1;
                UInt32 bytes = sizeof(running);
                state.error = AudioQueueGetProperty(state.value, kAudioQueueProperty_IsRunning, &running, &bytes);
                if (state.error != noErr) break;
                if (!running) { result = 0; break; }
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, false);
        }
    } else if (state.error == noErr && (interrupted || cancel_ms == 0)) result = 3;
    state.accepting = false;
    if (state.value) {
        if (result != 0 && AudioQueueStop(state.value, true) != noErr) result = 2;
        const OSStatus disposed = AudioQueueDispose(state.value, true);
        state.value = nullptr;
        if (disposed != noErr) { std::cerr << "queue disposal failed: " << disposed << '\n'; result = 2; }
    }
    CFRelease(uid);
    std::cout << "submitted_bytes=" << state.submitted << " expected_bytes=" << pcm.size() * sizeof(float) << " audioqueue_status=" << state.error << '\n';
    return result;
}
