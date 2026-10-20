// Shared-memory frame format between the OAL capture layer (inside vrcompositor)
// and the OAL Frame daemon. Both sides must agree on this layout.
//
// The layer writes a small, downsampled copy of each captured swapchain view
// (a coarse grid of linear-light RGBA floats) and the daemon reads it.
// Each view is protected by its own seqlock: `seq` is odd while the writer is
// updating the view, even when the view is stable.

#pragma once

#include <atomic>
#include <cstdint>

#define OAL_SHM_NAME "/oal-capture"
#define OAL_SHM_MAGIC "OALCAP1"
#define OAL_SHM_VERSION 2u

#define OAL_MAX_VIEWS 8u            // 4 swapchain slots x 2 array layers
#define OAL_MAX_GRID_DIM 320u       // grid is at most 320 x 320 cells (OAL_CAPTURE_GRID picks the actual cap)
#define OAL_VIEW_FLOATS (OAL_MAX_GRID_DIM * OAL_MAX_GRID_DIM * 4u)

enum OalViewFlags : uint32_t
{
    OAL_VIEW_LINEAR = 1u << 0,      // grid values are linear light (source was an _SRGB format)
};

// Plain (copyable) description of one captured view.
struct OalViewInfo
{
    uint32_t swapchainSlot;         // which tracked swapchain (0..3)
    uint32_t swapchainSerial;       // increments each time a swapchain is (re)created
    uint32_t layer;                 // array layer of the swapchain image
    uint32_t srcWidth, srcHeight;   // swapchain image size
    uint32_t srcLayers;             // swapchain imageArrayLayers
    uint32_t gridWidth, gridHeight; // size of the downsampled grid
    uint32_t vkFormat;              // VkFormat of the swapchain
    uint32_t vkColorSpace;          // VkColorSpaceKHR of the swapchain
    uint32_t flags;                 // OalViewFlags
    uint32_t _pad;
    uint64_t frameCounter;          // number of captures written to this view
    uint64_t timestampNs;           // CLOCK_MONOTONIC time of the capture
    uint32_t _reserved[8];
};

struct OalView
{
    std::atomic<uint32_t> seq;      // seqlock counter
    uint32_t _pad;
    OalViewInfo info;
};

struct OalShmHeader
{
    char magic[8];
    uint32_t version;
    uint32_t headerSize;
    uint32_t pid;                   // vrcompositor pid that last wrote
    uint32_t maxViews;
    uint32_t maxGridDim;
    uint32_t _pad;
    uint64_t startedNs;             // CLOCK_MONOTONIC when the writer attached
    uint32_t _reserved[16];
    OalView views[OAL_MAX_VIEWS];
};

// Layout: [OalShmHeader, padded to 4096][view 0 floats][view 1 floats]...
static constexpr uint32_t kOalHeaderBytes = 4096;
static constexpr uint64_t kOalShmBytes = kOalHeaderBytes + uint64_t(OAL_MAX_VIEWS) * OAL_VIEW_FLOATS * sizeof(float);

static_assert(sizeof(OalShmHeader) <= kOalHeaderBytes, "header too large");

inline float* OalViewData(void* base, uint32_t view)
{
    return reinterpret_cast<float*>(static_cast<char*>(base) + kOalHeaderBytes) + uint64_t(view) * OAL_VIEW_FLOATS;
}
inline const float* OalViewData(const void* base, uint32_t view)
{
    return reinterpret_cast<const float*>(static_cast<const char*>(base) + kOalHeaderBytes) + uint64_t(view) * OAL_VIEW_FLOATS;
}
