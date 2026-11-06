#include "preview-output.h"

#include <obs.h>
#include <cstring>
#include <exception>

namespace lbm {
bool play_preview_on_device(const std::vector<float> &, uint32_t, const std::string &, const std::atomic<bool> &,
                            std::string &);

std::vector<PreviewDevice> preview_devices()
{
    std::vector<PreviewDevice> devices;
    obs_enum_audio_monitoring_devices([](void *data, const char *name, const char *id) {
        if (id && *id && std::strcmp(id, "default") != 0)
            static_cast<std::vector<PreviewDevice> *>(data)->push_back({id, name && *name ? name : id});
        return true;
    }, &devices);
    return devices;
}

bool play_preview(const std::vector<float> &pcm, uint32_t rate, const std::string &id,
                  const std::atomic<bool> &cancel, std::string &error)
{
    error.clear();
    if (cancel.load()) return false;
    if (pcm.empty() || pcm.size() % 2 || !rate || id.empty() || id == "default" || id.front() == '@') {
        error = "Invalid stereo recording or output device";
        return false;
    }
    try {
        return play_preview_on_device(pcm, rate, id, cancel, error);
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}
} // namespace lbm
