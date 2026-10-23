// FSRD exposure tracking, pass 2 (one thread group): the median over the tiles of log2(current / previous
// brightness) is how much the game's exposure changed since last frame. A median, so moving characters, lights
// switching and disocclusions in a minority of the screen don't count. The tracked exposure is the product of those
// changes, pulled slowly back to 1 so a small bias in the estimate can't drift it away.

#include "fsrd_common.hlsli"

Texture2D<float4> Tiles : register(t0);
RWTexture2D<float> Exposure : register(u0); // 1x1, persists between frames

static const uint MaxTiles = 1024;
static const float MinTileTexels = 512.0f;   // of 4096: enough matching texels to trust the tile
static const float MaxStepLog2 = 1.0f;       // a frame's change is at most 2x either way
static const float MinValidShare = 0.25f;    // of the tiles, else (camera cut, fast turn) no change is measured
static const float Decay = 0.998f;           // per frame on log2: ~8 s half life at 45 fps
static const float MaxExposureLog2 = 6.0f;

groupshared float Values[MaxTiles];
groupshared uint ValidCount;

[numthreads(1024, 1, 1)]
void CSMain(uint index : SV_GroupIndex)
{
    const uint tileCount = min(ExposureTilesX * ExposureTilesY, MaxTiles);

    if (index == 0)
        ValidCount = 0;

    GroupMemoryBarrierWithGroupSync();

    // Invalid tiles sort to the end as +inf-ish
    float value = 1e30f;

    if (index < tileCount)
    {
        const float4 t = Tiles.Load(int3(index % ExposureTilesX, index / ExposureTilesX, 0));

        if (t.z >= MinTileTexels && t.x > 1e-5f && t.y > 1e-5f)
        {
            value = clamp(log2(t.x / t.y), -MaxStepLog2, MaxStepLog2);
            InterlockedAdd(ValidCount, 1);
        }
    }

    Values[index] = value;
    GroupMemoryBarrierWithGroupSync();

    // Bitonic sort, ascending
    for (uint k = 2; k <= MaxTiles; k <<= 1)
    {
        for (uint j = k >> 1; j > 0; j >>= 1)
        {
            const uint other = index ^ j;

            if (other > index)
            {
                const bool ascending = (index & k) == 0;
                const float a = Values[index];
                const float b = Values[other];

                if ((a > b) == ascending)
                {
                    Values[index] = b;
                    Values[other] = a;
                }
            }

            GroupMemoryBarrierWithGroupSync();
        }
    }

    if (index != 0)
        return;

    float exposureLog2 = 0.0f;

    if ((Flags & FLAG_EXPOSURE_RESET) == 0)
    {
        const float previous = Exposure[uint2(0, 0)];
        exposureLog2 = (previous > 0.0f && !isnan(previous)) ? log2(previous) : 0.0f;

        if (ValidCount >= max(8u, (uint) (MinValidShare * tileCount)))
            exposureLog2 += Values[ValidCount / 2];

        exposureLog2 = clamp(exposureLog2 * Decay, -MaxExposureLog2, MaxExposureLog2);
    }

    Exposure[uint2(0, 0)] = exp2(exposureLog2);
}
