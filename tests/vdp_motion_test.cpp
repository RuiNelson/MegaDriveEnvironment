#include "system/graphics/VDPMotion.hpp"

#include <cassert>
#include <memory>
#include <vector>

// computeMotion: where each pixel was in the previous frame, minus where it is now.
namespace {

float motionX(const std::vector<float> &motion, int width, int x, int y) {
    return motion[(static_cast<std::size_t>(y) * width + x) * 2];
}
float motionY(const std::vector<float> &motion, int width, int x, int y) {
    return motion[(static_cast<std::size_t>(y) * width + x) * 2 + 1];
}

void fill(VDPFrameLayers &layers, std::uint8_t source) {
    for (int y = 0; y < layers.height; ++y)
        for (int x = 0; x < layers.width; ++x)
            layers.source[static_cast<std::size_t>(y) * VDPState::MAX_SCREEN_W + x] = source;
}

} // namespace

int main() {
    constexpr int W = 320, H = 224;
    auto          previous = std::make_unique<VDPFrameLayers>();
    auto          current  = std::make_unique<VDPFrameLayers>();
    previous->begin(W, H, 512, 256, 0);
    current->begin(W, H, 512, 256, 0);
    std::vector<float> motion(static_cast<std::size_t>(W) * H * 2);

    // Plane A scrolled 3 px left (hscroll -10 to -13) and 2 px up (vscroll 4 to 6): a pixel now at x was at
    // x + 3, y + 2. Plane B, wrapping from 510 to 2 (4 px right on a 512 px plane), was 4 px to the left.
    fill(*previous, VDPFrameLayers::PlaneA);
    fill(*current, VDPFrameLayers::PlaneA);
    for (int y = 0; y < H; ++y) {
        previous->lines[y].hscrollA = -10;
        current->lines[y].hscrollA  = -13;
        previous->lines[y].vsram[0] = 4;
        current->lines[y].vsram[0]  = 6;
        previous->lines[y].hscrollB = 510;
        current->lines[y].hscrollB  = 2;
    }
    current->source[5 * VDPState::MAX_SCREEN_W + 7] = VDPFrameLayers::PlaneB;
    current->source[6 * VDPState::MAX_SCREEN_W + 7] = VDPFrameLayers::Window;
    current->source[7 * VDPState::MAX_SCREEN_W + 7] = VDPFrameLayers::Overlay;
    computeMotion(*previous, *current, motion.data());
    assert(motionX(motion, W, 100, 100) == 3.0f && motionY(motion, W, 100, 100) == 2.0f);
    assert(motionX(motion, W, 7, 5) == -4.0f && motionY(motion, W, 7, 5) == 0.0f);
    assert(motionX(motion, W, 7, 6) == 0.0f && motionY(motion, W, 7, 6) == 0.0f); // the window stays
    assert(motionX(motion, W, 7, 7) == 0.0f && motionY(motion, W, 7, 7) == 0.0f); // host drawing stays

    // 2-cell vertical scroll: column 1 (x 16-31) of plane A moved, column 0 did not.
    for (int y = 0; y < H; ++y) {
        previous->lines[y]                = {};
        current->lines[y]                 = {};
        previous->lines[y].twoCellVscroll = current->lines[y].twoCellVscroll = true;
        current->lines[y].vsram[2]        = 5; // column 1, plane A
    }
    computeMotion(*previous, *current, motion.data());
    assert(motionY(motion, W, 8, 50) == 0.0f && motionY(motion, W, 20, 50) == 5.0f);

    // A sprite that moved 6 px right with the same pattern, one whose pattern changed in the same SAT slot,
    // and one with neither, which stays.
    for (int y = 0; y < H; ++y) {
        previous->lines[y] = {};
        current->lines[y]  = {};
    }
    previous->begin(W, H, 512, 256, 0);
    current->begin(W, H, 512, 256, 0);
    fill(*current, VDPFrameLayers::Backdrop);
    previous->sprites[9]  = {true, 100, 50, 16, 32, 0x200, 1, false, false};
    current->sprites[3]   = {true, 106, 50, 16, 32, 0x200, 1, false, false};
    previous->sprites[4]  = {true, 40, 60, 24, 24, 0x300, 2, false, false};
    current->sprites[4]   = {true, 37, 61, 24, 24, 0x318, 2, false, false};
    current->sprites[20]  = {true, 200, 100, 8, 8, 0x400, 0, false, false};
    current->source[55 * VDPState::MAX_SCREEN_W + 110] = VDPFrameLayers::SpriteBase + 3;
    current->source[65 * VDPState::MAX_SCREEN_W + 45]  = VDPFrameLayers::SpriteBase + 4;
    current->source[102 * VDPState::MAX_SCREEN_W + 202] = VDPFrameLayers::SpriteBase + 20;
    computeMotion(*previous, *current, motion.data());
    assert(motionX(motion, W, 110, 55) == -6.0f && motionY(motion, W, 110, 55) == 0.0f);
    assert(motionX(motion, W, 45, 65) == 3.0f && motionY(motion, W, 45, 65) == -1.0f);
    assert(motionX(motion, W, 202, 102) == 0.0f && motionY(motion, W, 202, 102) == 0.0f);
    assert(motionX(motion, W, 0, 0) == 0.0f); // backdrop

    // Pose holds. Sprite 4 changed pattern and moved 4 px left (x 40 to 36), so a frame halfway holds it 2 px
    // from each end, taken from the previous frame 2 px to the right of where it shows: its old pixel at 41 shows
    // at 39, its new pixel at 50 at 52. Sprite 3 kept its pattern: interpolated, no hold. Sprite 20 is new and
    // sprite 30 is gone: each holds where it is, offset 0.
    {
        auto then = std::make_unique<VDPFrameLayers>();
        auto now  = std::make_unique<VDPFrameLayers>();
        then->begin(W, H, 512, 256, 0);
        now->begin(W, H, 512, 256, 0);
        fill(*then, VDPFrameLayers::Backdrop);
        fill(*now, VDPFrameLayers::Backdrop);
        then->sprites[4]  = {true, 40, 60, 24, 24, 0x300, 2, false, false};
        now->sprites[4]   = {true, 36, 60, 24, 24, 0x318, 2, false, false};
        then->sprites[9]  = {true, 100, 50, 16, 32, 0x200, 1, false, false};
        now->sprites[3]   = {true, 106, 50, 16, 32, 0x200, 1, false, false};
        now->sprites[20]  = {true, 200, 100, 8, 8, 0x400, 0, false, false};
        then->sprites[30] = {true, 250, 150, 8, 8, 0x500, 0, false, false};
        then->source[65 * VDPState::MAX_SCREEN_W + 41]   = VDPFrameLayers::SpriteBase + 4;
        now->source[65 * VDPState::MAX_SCREEN_W + 50]    = VDPFrameLayers::SpriteBase + 4;
        then->source[55 * VDPState::MAX_SCREEN_W + 104]  = VDPFrameLayers::SpriteBase + 9;
        now->source[55 * VDPState::MAX_SCREEN_W + 110]   = VDPFrameLayers::SpriteBase + 3;
        now->source[102 * VDPState::MAX_SCREEN_W + 202]  = VDPFrameLayers::SpriteBase + 20;
        then->source[152 * VDPState::MAX_SCREEN_W + 251] = VDPFrameLayers::SpriteBase + 30;
        std::vector<float> holds(static_cast<std::size_t>(W) * H * 4);
        assert(computePoseHolds(*then, *now, holds.data()));
        auto at = [&](int x, int y) { return holds.data() + (static_cast<std::size_t>(y) * W + x) * 4; };
        assert(at(39, 65)[2] == 1.0f && at(39, 65)[0] == 2.0f && at(39, 65)[1] == 0.0f);
        assert(at(52, 65)[2] == 1.0f && at(52, 65)[0] == 2.0f);
        assert(at(41, 65)[2] == 0.0f && at(50, 65)[2] == 0.0f);
        assert(at(110, 55)[2] == 0.0f && at(107, 55)[2] == 0.0f && at(104, 55)[2] == 0.0f);
        assert(at(202, 102)[2] == 1.0f && at(202, 102)[0] == 0.0f);
        assert(at(251, 152)[2] == 1.0f && at(251, 152)[0] == 0.0f);
        assert(at(0, 0)[2] == 0.0f);

        // Nothing changed pattern, appeared or went: nothing holds.
        now->sprites[4]   = then->sprites[4];
        now->sprites[20]  = {};
        then->sprites[30] = {};
        now->source[102 * VDPState::MAX_SCREEN_W + 202] = VDPFrameLayers::Backdrop;
        assert(!computePoseHolds(*then, *now, holds.data()));
    }

    // Another picture size: no motion at all.
    current->begin(256, 224, 512, 256, 0);
    fill(*current, VDPFrameLayers::PlaneA);
    current->lines[0].hscrollA = 50;
    std::vector<float> narrow(static_cast<std::size_t>(256) * 224 * 2, 9.0f);
    computeMotion(*previous, *current, narrow.data());
    assert(narrow[0] == 0.0f && narrow[1] == 0.0f);
    return 0;
}
