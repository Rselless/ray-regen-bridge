#pragma once
// Read-only log of what a game sends to its own FSR Ray Regeneration (FidelityFX denoiser) context, for games that
// use the denoiser natively (e.g. Crimson Desert). The calls are passed through unchanged by FfxApi_Dx12.cpp, this
// only describes them: context creation, every configured value, and the first dispatch plus any later dispatch
// whose shape (signals, flags, sizes, formats) changes.

#include <ffx_api.h>

namespace FfxDenoiserLog
{
// True if the desc chain belongs to the denoiser or the radiance cache effect
bool IsDenoiserChain(const ffxApiHeader* header);

void LogCreate(const ffxApiHeader* header, ffxReturnCode_t result, ffxContext context);
void LogConfigure(const ffxApiHeader* header, ffxReturnCode_t result);
void LogQuery(const ffxApiHeader* header, ffxReturnCode_t result);
void LogDispatch(const ffxApiHeader* header, ffxReturnCode_t result);
} // namespace FfxDenoiserLog
