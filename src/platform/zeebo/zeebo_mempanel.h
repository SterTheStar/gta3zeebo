// zeebo_mempanel.h - Top-left on-screen memory & performance diagnostics panel for Zeebo.
#pragma once

#ifdef __ZEEBO__

#include <stdint.h>

namespace ZeeboMemPanel {

void SetEnabled(bool enabled);
bool IsEnabled(void);
void Toggle(void);

// Feed per-window (e.g. 30-frame) profiling metrics from Idle().
void UpdateFrameStats(uint32_t realFps, uint32_t frameMs, uint32_t drawCalls, uint32_t tris);

// Draw the top-left overlay panel (call before CFont::DrawFonts()).
void Draw(void);

} // namespace ZeeboMemPanel

#endif // __ZEEBO__
