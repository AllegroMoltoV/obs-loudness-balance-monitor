#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(OSStatus status)
{
	if (status != noErr)
		throw std::runtime_error("CoreAudio status=" + std::to_string(status));
}

static std::string string_property(AudioDeviceID id, AudioObjectPropertySelector selector)
{
	AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	CFStringRef value = nullptr;
	UInt32 size = sizeof(value);
	check(AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &value));
	if (!value)
		throw std::runtime_error("Missing device string");
	std::vector<char> encoded(CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) +
				  1);
	const bool converted = CFStringGetCString(value, encoded.data(), encoded.size(), kCFStringEncodingUTF8);
	CFRelease(value);
	if (!converted)
		throw std::runtime_error("Invalid device string");
	return encoded.data();
}

int main(int argc, char **argv)
{
	try {
		const bool use_default = argc == 2 && std::string(argv[1]) == "--default";
		if (argc != 1 && !use_default)
			throw std::runtime_error("Usage: macos-output-device [--default]");
		AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
						   kAudioObjectPropertyElementMain};
		UInt32 size = 0;
		check(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size));
		if (!size)
			throw std::runtime_error("No CoreAudio devices");
		std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
		check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
						 devices.data()));
		devices.resize(size / sizeof(AudioDeviceID));
		std::vector<std::string> candidates;
		for (const auto id : devices) {
			const auto name = string_property(id, kAudioObjectPropertyName);
			const auto uid = string_property(id, kAudioDevicePropertyDeviceUID);
			address = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
				   kAudioObjectPropertyElementMain};
			UInt32 alive = 0;
			size = sizeof(alive);
			check(AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &alive));
			address = {kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput,
				   kAudioObjectPropertyElementMain};
			size = 0;
			check(AudioObjectGetPropertyDataSize(id, &address, 0, nullptr, &size));
			std::cerr << "id=" << id << " name=" << name << " uid=" << uid << " alive=" << alive
				  << " output_streams=" << size / sizeof(AudioStreamID) << '\n';
			if (name == "Apple Virtual Sound Device" && alive && size && !uid.empty())
				candidates.push_back(uid);
		}
		if (candidates.size() != 1)
			throw std::runtime_error("Expected one active Apple Virtual Sound Device output, found " +
						 std::to_string(candidates.size()));
		if (use_default) {
			address = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
				   kAudioObjectPropertyElementMain};
			AudioDeviceID device = kAudioObjectUnknown;
			size = sizeof(device);
			check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
							 &device));
			if (device == kAudioObjectUnknown)
				throw std::runtime_error("No default output device");
			const auto uid = string_property(device, kAudioDevicePropertyDeviceUID);
			if (uid != candidates.front())
				throw std::runtime_error(
					"Default output is not the approved Apple Virtual Sound Device");
			std::cerr << "default_output_id=" << device << " uid=" << uid << '\n';
			std::cout << uid << '\n';
		} else {
			std::cout << candidates.front() << '\n';
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
