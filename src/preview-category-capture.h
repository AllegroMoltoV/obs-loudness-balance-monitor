#pragma once

#include <obs.h>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace lbm {

struct CategoryBlock {
    uint64_t timestamp = 0;
    uint32_t frames = 0;
    std::array<float, AUDIO_OUTPUT_FRAMES * 2> voice{};
    std::array<float, AUDIO_OUTPUT_FRAMES * 2> bgm{};
};

void register_preview_category_capture();

// Control-thread start/stop; consume_raw runs on OBS's audio render thread.
// One worker calls pop. Detach raw callback and stop the worker before stop().
class PreviewCategoryCapture {
public:
    PreviewCategoryCapture();
    ~PreviewCategoryCapture();
    PreviewCategoryCapture(const PreviewCategoryCapture &) = delete;
    PreviewCategoryCapture &operator=(const PreviewCategoryCapture &) = delete;

    bool start(const std::string &voice, const std::vector<std::string> &bgm,
               unsigned mix_index, uint32_t rate);
    void consume_raw(const audio_data &packet) noexcept;
    bool pop(CategoryBlock &block) noexcept;
    // Stable literal, empty on success; safe to poll while capturing.
    const char *error() const noexcept;
    void stop();

private:
    struct State;
    std::shared_ptr<State> state_;
    obs_source_t *observer_ = nullptr;
    friend void register_preview_category_capture();
};

} // namespace lbm
