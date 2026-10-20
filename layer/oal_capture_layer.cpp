// OAL capture layer: a small Vulkan implicit layer for SteamVR's vrcompositor
// on the Steam Frame.
//
// At a low rate (default 30 Hz) it copies the image the compositor is about to
// present, downsamples it with a chain of 2:1 GPU blits (a box filter, no
// shaders), reads the small result back and publishes it to shared memory
// (see common/oal_shm.h). The OAL Frame daemon turns that into LED colours.
//
// Safety rules this layer follows:
//  * It only does anything inside a process named "vrcompositor" (or when
//    OAL_CAPTURE_ANY_PROCESS=1 is set, for testing). Everywhere else it is a
//    pure pass-through.
//  * It never blocks the compositor. If a readback is not finished it skips
//    the capture rather than waiting.
//  * On any failure it disables capture for that swapchain and passes
//    presents through unchanged. If the swapchain cannot be created with the
//    extra usage flag it needs, it retries with the compositor's original
//    request.
//  * The file ~/.config/oal-frame/layer-disabled turns it into a pass-through.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/oal_shm.h"

#define OAL_EXPORT extern "C" __attribute__((visibility("default")))

namespace
{
constexpr uint32_t kRingSize = 3;
constexpr uint32_t kMaxSwapchainSlots = 4;
constexpr uint32_t kMaxLayersCaptured = 2;

// ---------------------------------------------------------------------------
// Utilities

uint64_t NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

template <typename T> void* Key(T handle) { return *reinterpret_cast<void**>(handle); }

FILE* g_log = nullptr;
std::mutex g_logMutex;
unsigned g_logLines = 0;

void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> l(g_logMutex);
    if (!g_log || g_logLines > 2000) return; // keep the log small
    g_logLines++;
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
    fprintf(g_log, "%s [%d] ", tbuf, int(getpid()));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

std::string HomeDir()
{
    const char* h = getenv("HOME");
    return h ? std::string(h) : std::string();
}

bool FileExists(const std::string& p)
{
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0;
}

void OpenLog()
{
    std::string dir;
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) dir = std::string(rt) + "/oal-frame";
    else dir = "/tmp/oal-frame-" + std::to_string(getuid());
    mkdir(dir.c_str(), 0700); // ignore errors; fopen below decides
    std::string path = dir + "/layer.log";
    // Never let a logging problem affect the compositor: on failure we just
    // run without a log.
    g_log = fopen(path.c_str(), "a");
}

bool IsTargetProcess()
{
    const char* any = getenv("OAL_CAPTURE_ANY_PROCESS");
    if (any && strcmp(any, "1") == 0) return true;
    char path[4096] = {};
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return false;
    path[n] = '\0';
    const char* base = strrchr(path, '/');
    return strcmp(base ? base + 1 : path, "vrcompositor") == 0;
}

bool IsSrgbFormat(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8_SRGB:
    case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_B8G8R8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

float HalfToFloat(uint16_t h)
{
    uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0)
    {
        if (mant == 0) bits = sign;
        else
        {
            // subnormal half -> normal float
            int e = -1;
            do { e++; mant <<= 1; } while ((mant & 0x400u) == 0);
            mant &= 0x3FFu;
            bits = sign | uint32_t(127 - 15 - e) << 23 | mant << 13;
        }
    }
    else if (exp == 31) bits = sign | 0x7F800000u | mant << 13;
    else bits = sign | (exp + (127 - 15)) << 23 | mant << 13;
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

// ---------------------------------------------------------------------------
// Global state

struct InstanceData
{
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
};

struct DeviceData
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    InstanceData* inst = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData SetDeviceLoaderData = nullptr;
    VkPhysicalDeviceMemoryProperties memProps = {};
    std::vector<VkQueueFamilyProperties> queueFamilies;
    std::unordered_map<VkQueue, uint32_t> queueFamilyOf;

#define OAL_DEVICE_FUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(CreateSwapchainKHR) X(DestroySwapchainKHR) \
    X(GetSwapchainImagesKHR) X(QueuePresentKHR) X(QueueSubmit) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdPipelineBarrier) X(CmdBlitImage) X(CmdCopyImageToBuffer) X(CreateImage) X(DestroyImage) \
    X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateBuffer) \
    X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
    X(InvalidateMappedMemoryRanges) X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(ResetFences) \
    X(WaitForFences) X(CreateSemaphore) X(DestroySemaphore)
#define OAL_DECLARE(name) PFN_vk##name name = nullptr;
    OAL_DEVICE_FUNCS(OAL_DECLARE)
#undef OAL_DECLARE
};

struct RingSlot
{
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool coherent = true;
    bool pending = false;
    uint64_t timestampNs = 0;
};

struct SwapData
{
    DeviceData* dev = nullptr;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    bool captureOk = false;
    bool resourcesTried = false;
    bool resourcesOk = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent = {};
    uint32_t srcLayers = 1;
    uint32_t layers = 1; // layers we capture (<= kMaxLayersCaptured)
    bool linearFilter = true;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> semaphores;
    uint32_t slot = UINT32_MAX;
    uint32_t serial = 0;

    VkFormat gridFormat = VK_FORMAT_UNDEFINED;
    uint32_t gridTexelBytes = 0;
    std::vector<VkExtent2D> levels;
    VkImage mip = VK_NULL_HANDLE;
    VkDeviceMemory mipMemory = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;
    VkCommandPool pool = VK_NULL_HANDLE;
    RingSlot ring[kRingSize];
    uint64_t lastCaptureNs = 0;
    uint32_t failures = 0;
};

std::mutex g_mutex;
std::unordered_map<void*, InstanceData*> g_instances; // key: instance dispatch
std::unordered_map<void*, DeviceData*> g_devices;     // key: device dispatch
std::unordered_map<VkSwapchainKHR, SwapData*> g_swaps;

bool g_initDone = false;
bool g_target = false;
bool g_disabled = false;
uint64_t g_intervalNs = 1000000000ull / 30;
uint32_t g_gridCap = 128; // OAL_CAPTURE_GRID
OalShmHeader* g_shm = nullptr;
uint32_t g_serialCounter = 0;
bool g_slotUsed[kMaxSwapchainSlots] = {};
uint32_t g_roundRobin = 0;

void GlobalInit()
{
    if (g_initDone) return;
    g_initDone = true;
    g_target = IsTargetProcess();
    if (!g_target) return;

    OpenLog();
    std::string killSwitch = HomeDir() + "/.config/oal-frame/layer-disabled";
    if (FileExists(killSwitch))
    {
        g_disabled = true;
        Log("layer loaded but disabled by %s", killSwitch.c_str());
        return;
    }

    const char* hz = getenv("OAL_CAPTURE_HZ");
    if (hz)
    {
        double v = atof(hz);
        if (v >= 1.0 && v <= 240.0) g_intervalNs = uint64_t(1e9 / v);
    }

    const char* grid = getenv("OAL_CAPTURE_GRID");
    if (grid)
    {
        long v = atol(grid);
        if (v >= 8 && v <= long(OAL_MAX_GRID_DIM)) g_gridCap = uint32_t(v);
    }

    int fd = shm_open(OAL_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
    {
        Log("shm_open failed (%s); capture disabled", strerror(errno));
        g_disabled = true;
        return;
    }
    if (ftruncate(fd, off_t(kOalShmBytes)) != 0)
    {
        Log("ftruncate failed (%s); capture disabled", strerror(errno));
        close(fd);
        g_disabled = true;
        return;
    }
    void* p = mmap(nullptr, kOalShmBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
    {
        Log("mmap failed (%s); capture disabled", strerror(errno));
        g_disabled = true;
        return;
    }
    g_shm = static_cast<OalShmHeader*>(p);
    if (memcmp(g_shm->magic, OAL_SHM_MAGIC, 8) != 0 || g_shm->version != OAL_SHM_VERSION)
    {
        memset(p, 0, kOalHeaderBytes);
        memcpy(g_shm->magic, OAL_SHM_MAGIC, 8);
        g_shm->version = OAL_SHM_VERSION;
        g_shm->headerSize = kOalHeaderBytes;
        g_shm->maxViews = OAL_MAX_VIEWS;
        g_shm->maxGridDim = OAL_MAX_GRID_DIM;
    }
    g_shm->pid = uint32_t(getpid());
    g_shm->startedNs = NowNs();
    Log("layer active in pid %d, capture interval %.1f ms, grid cap %u", int(getpid()), double(g_intervalNs) / 1e6, g_gridCap);
}

bool Active() { return g_target && !g_disabled && g_shm; }

DeviceData* DeviceOf(void* key)
{
    auto it = g_devices.find(key);
    return it == g_devices.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Capture resources

uint32_t FindMemoryType(const DeviceData* d, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags* got)
{
    for (uint32_t i = 0; i < d->memProps.memoryTypeCount; i++)
    {
        if ((bits & (1u << i)) && (d->memProps.memoryTypes[i].propertyFlags & want) == want)
        {
            if (got) *got = d->memProps.memoryTypes[i].propertyFlags;
            return i;
        }
    }
    return UINT32_MAX;
}

void DestroyCaptureResources(SwapData* s)
{
    DeviceData* d = s->dev;
    VkDevice dev = d->device;
    std::vector<VkFence> fences;
    for (auto& r : s->ring)
        if (r.fence) fences.push_back(r.fence);
    if (!fences.empty()) d->WaitForFences(dev, uint32_t(fences.size()), fences.data(), VK_TRUE, 200000000ull);
    for (auto& r : s->ring)
    {
        if (r.mapped) d->UnmapMemory(dev, r.memory);
        if (r.buffer) d->DestroyBuffer(dev, r.buffer, nullptr);
        if (r.memory) d->FreeMemory(dev, r.memory, nullptr);
        if (r.fence) d->DestroyFence(dev, r.fence, nullptr);
        r = RingSlot();
    }
    if (s->pool) d->DestroyCommandPool(dev, s->pool, nullptr); // frees command buffers
    if (s->mip) d->DestroyImage(dev, s->mip, nullptr);
    if (s->mipMemory) d->FreeMemory(dev, s->mipMemory, nullptr);
    s->pool = VK_NULL_HANDLE;
    s->mip = VK_NULL_HANDLE;
    s->mipMemory = VK_NULL_HANDLE;
    s->resourcesOk = false;
}

bool FormatHas(DeviceData* d, VkFormat f, VkFormatFeatureFlags feats)
{
    VkFormatProperties props = {};
    d->inst->GetPhysicalDeviceFormatProperties(d->phys, f, &props);
    return (props.optimalTilingFeatures & feats) == feats;
}

bool CreateCaptureResources(SwapData* s, uint32_t queueFamily)
{
    DeviceData* d = s->dev;
    VkDevice dev = d->device;
    s->resourcesTried = true;
    s->queueFamily = queueFamily;

    if (queueFamily >= d->queueFamilies.size() || !(d->queueFamilies[queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    {
        Log("swapchain %u: present queue family %u has no graphics support; capture off", s->serial, queueFamily);
        return false;
    }

    const VkFormatFeatureFlags gridFeats =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (FormatHas(d, VK_FORMAT_R16G16B16A16_SFLOAT, gridFeats))
    {
        s->gridFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        s->gridTexelBytes = 8;
    }
    else if (FormatHas(d, VK_FORMAT_R8G8B8A8_UNORM, gridFeats))
    {
        s->gridFormat = VK_FORMAT_R8G8B8A8_UNORM;
        s->gridTexelBytes = 4;
    }
    else
    {
        Log("swapchain %u: no usable grid format; capture off", s->serial);
        return false;
    }

    // 2:1 chain: level 0 is half the swapchain size, keep halving until the
    // grid fits in g_gridCap (OAL_CAPTURE_GRID, at most OAL_MAX_GRID_DIM).
    s->levels.clear();
    uint32_t w = std::max(1u, s->extent.width / 2), h = std::max(1u, s->extent.height / 2);
    s->levels.push_back({w, h});
    while ((w > g_gridCap || h > g_gridCap) && s->levels.size() < 16)
    {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        s->levels.push_back({w, h});
    }

    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = s->gridFormat;
    ici.extent = {s->levels[0].width, s->levels[0].height, 1};
    ici.mipLevels = uint32_t(s->levels.size());
    ici.arrayLayers = s->layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d->CreateImage(dev, &ici, nullptr, &s->mip) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    d->GetImageMemoryRequirements(dev, s->mip, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemoryType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = FindMemoryType(d, mr.memoryTypeBits, 0, nullptr);
    if (mai.memoryTypeIndex == UINT32_MAX) return false;
    if (d->AllocateMemory(dev, &mai, nullptr, &s->mipMemory) != VK_SUCCESS) return false;
    if (d->BindImageMemory(dev, s->mip, s->mipMemory, 0) != VK_SUCCESS) return false;

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    if (d->CreateCommandPool(dev, &pci, nullptr, &s->pool) != VK_SUCCESS) return false;

    const VkExtent2D last = s->levels.back();
    const VkDeviceSize bufBytes = VkDeviceSize(last.width) * last.height * s->gridTexelBytes * s->layers;

    for (auto& r : s->ring)
    {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = s->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (d->AllocateCommandBuffers(dev, &cai, &r.cb) != VK_SUCCESS) return false;
        // Command buffers made by a layer need the loader's dispatch pointer.
        if (d->SetDeviceLoaderData) d->SetDeviceLoaderData(dev, r.cb);
        else *reinterpret_cast<void**>(r.cb) = *reinterpret_cast<void**>(dev);

        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (d->CreateFence(dev, &fci, nullptr, &r.fence) != VK_SUCCESS) return false;

        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bufBytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (d->CreateBuffer(dev, &bci, nullptr, &r.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements bmr;
        d->GetBufferMemoryRequirements(dev, r.buffer, &bmr);
        VkMemoryPropertyFlags got = 0;
        uint32_t type = FindMemoryType(d, bmr.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                       &got);
        if (type == UINT32_MAX)
            type = FindMemoryType(d, bmr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &got);
        if (type == UINT32_MAX) type = FindMemoryType(d, bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &got);
        if (type == UINT32_MAX) return false;
        r.coherent = (got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bai.allocationSize = bmr.size;
        bai.memoryTypeIndex = type;
        if (d->AllocateMemory(dev, &bai, nullptr, &r.memory) != VK_SUCCESS) return false;
        if (d->BindBufferMemory(dev, r.buffer, r.memory, 0) != VK_SUCCESS) return false;
        if (d->MapMemory(dev, r.memory, 0, VK_WHOLE_SIZE, 0, &r.mapped) != VK_SUCCESS) return false;
    }

    Log("swapchain %u: capture ready, grid %ux%u (%u blit levels), grid format %d, %u layer(s), queue family %u",
        s->serial, last.width, last.height, unsigned(s->levels.size()), int(s->gridFormat), s->layers, queueFamily);
    s->resourcesOk = true;
    return true;
}

void RecordCapture(SwapData* s, RingSlot& r, VkImage src)
{
    DeviceData* d = s->dev;
    VkCommandBuffer cb = r.cb;
    d->ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(cb, &bi);

    const uint32_t nLevels = uint32_t(s->levels.size());

    // Swapchain image: PRESENT_SRC -> TRANSFER_SRC (all layers), and the whole
    // grid chain -> TRANSFER_DST (contents discarded).
    VkImageMemoryBarrier pre[2] = {};
    pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[0].image = src;
    pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    pre[1] = pre[0];
    pre[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    pre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pre[1].image = s->mip;
    pre[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, nLevels, 0, s->layers};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 2, pre);

    // Swapchain -> level 0
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s->layers};
    blit.srcOffsets[1] = {int32_t(s->extent.width), int32_t(s->extent.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s->layers};
    blit.dstOffsets[1] = {int32_t(s->levels[0].width), int32_t(s->levels[0].height), 1};
    d->CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->mip, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                    s->linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

    // Level i-1 -> level i
    for (uint32_t i = 1; i <= nLevels; i++)
    {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s->mip;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, s->layers};
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                              nullptr, 1, &b);
        if (i == nLevels) break;
        VkImageBlit lb = {};
        lb.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, s->layers};
        lb.srcOffsets[1] = {int32_t(s->levels[i - 1].width), int32_t(s->levels[i - 1].height), 1};
        lb.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, s->layers};
        lb.dstOffsets[1] = {int32_t(s->levels[i].width), int32_t(s->levels[i].height), 1};
        d->CmdBlitImage(cb, s->mip, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->mip, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                        &lb, VK_FILTER_LINEAR);
    }

    // Last level -> host buffer (layers are packed one after another)
    const VkExtent2D last = s->levels.back();
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, nLevels - 1, 0, s->layers};
    copy.imageExtent = {last.width, last.height, 1};
    d->CmdCopyImageToBuffer(cb, s->mip, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.buffer, 1, &copy);

    // Give the swapchain image back in the layout the compositor left it in.
    VkImageMemoryBarrier post = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    post.srcAccessMask = 0;
    post.dstAccessMask = 0;
    post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = src;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    VkBufferMemoryBarrier host = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = r.buffer;
    host.size = VK_WHOLE_SIZE;
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, nullptr, 1, &host, 1, &post);
    d->EndCommandBuffer(cb);
}

void Publish(SwapData* s, RingSlot& r)
{
    if (!g_shm || s->slot >= kMaxSwapchainSlots) return;
    DeviceData* d = s->dev;
    if (!r.coherent)
    {
        VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = r.memory;
        range.size = VK_WHOLE_SIZE;
        d->InvalidateMappedMemoryRanges(d->device, 1, &range);
    }
    const VkExtent2D g = s->levels.back();
    const uint32_t texels = g.width * g.height;
    const uint8_t* src = static_cast<const uint8_t*>(r.mapped);

    for (uint32_t l = 0; l < s->layers; l++)
    {
        uint32_t vi = s->slot * kMaxLayersCaptured + l;
        if (vi >= OAL_MAX_VIEWS) break;
        OalView& v = g_shm->views[vi];
        float* dst = OalViewData(g_shm, vi);
        uint32_t seq = v.seq.load(std::memory_order_relaxed);
        v.seq.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        const uint8_t* layerSrc = src + size_t(l) * texels * s->gridTexelBytes;
        if (s->gridFormat == VK_FORMAT_R16G16B16A16_SFLOAT)
        {
            const uint16_t* h = reinterpret_cast<const uint16_t*>(layerSrc);
            for (uint32_t i = 0; i < texels * 4; i++) dst[i] = HalfToFloat(h[i]);
        }
        else
        {
            for (uint32_t i = 0; i < texels * 4; i++) dst[i] = layerSrc[i] / 255.0f;
        }
        v.info.swapchainSlot = s->slot;
        v.info.swapchainSerial = s->serial;
        v.info.layer = l;
        v.info.srcWidth = s->extent.width;
        v.info.srcHeight = s->extent.height;
        v.info.srcLayers = s->srcLayers;
        v.info.gridWidth = g.width;
        v.info.gridHeight = g.height;
        v.info.vkFormat = uint32_t(s->format);
        v.info.vkColorSpace = uint32_t(s->colorSpace);
        v.info.flags = IsSrgbFormat(s->format) ? OAL_VIEW_LINEAR : 0u;
        v.info.frameCounter++;
        v.info.timestampNs = r.timestampNs;

        std::atomic_thread_fence(std::memory_order_release);
        v.seq.store(seq + 2, std::memory_order_release);
    }
}

// Collect finished readbacks. Returns a free ring slot or nullptr.
RingSlot* PollRing(SwapData* s)
{
    DeviceData* d = s->dev;
    RingSlot* freeSlot = nullptr;
    RingSlot* newest = nullptr;
    for (auto& r : s->ring)
    {
        if (d->GetFenceStatus(d->device, r.fence) != VK_SUCCESS) continue; // still in flight
        if (r.pending)
        {
            r.pending = false;
            if (!newest || r.timestampNs > newest->timestampNs) newest = &r;
        }
        if (!freeSlot) freeSlot = &r;
    }
    if (newest) Publish(s, *newest);
    return freeSlot;
}

void ReleaseSlot(SwapData* s)
{
    if (s->slot < kMaxSwapchainSlots) g_slotUsed[s->slot] = false;
    s->slot = UINT32_MAX;
}

void DestroySwapData(SwapData* s)
{
    DeviceData* d = s->dev;
    DestroyCaptureResources(s);
    for (VkSemaphore sem : s->semaphores)
        if (sem) d->DestroySemaphore(d->device, sem, nullptr);
    ReleaseSlot(s);
    delete s;
}

// ---------------------------------------------------------------------------
// Intercepted functions

VKAPI_ATTR VkResult VKAPI_CALL oal_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
    VkLayerInstanceCreateInfo* chain = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo*)chain->pNext;
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = create(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    auto* id = new InstanceData();
    id->instance = *pInstance;
    id->gipa = gipa;
    id->DestroyInstance = (PFN_vkDestroyInstance)gipa(*pInstance, "vkDestroyInstance");
    id->GetPhysicalDeviceFormatProperties =
        (PFN_vkGetPhysicalDeviceFormatProperties)gipa(*pInstance, "vkGetPhysicalDeviceFormatProperties");
    id->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(*pInstance, "vkGetPhysicalDeviceMemoryProperties");
    id->GetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties");
    id->GetPhysicalDeviceSurfaceCapabilitiesKHR =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(*pInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    std::lock_guard<std::mutex> l(g_mutex);
    GlobalInit();
    g_instances[Key(*pInstance)] = id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL oal_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    if (!instance) return;
    InstanceData* id = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_instances.find(Key(instance));
        if (it == g_instances.end()) return;
        id = it->second;
        g_instances.erase(it);
    }
    id->DestroyInstance(instance, pAllocator);
    delete id;
}

VKAPI_ATTR VkResult VKAPI_CALL oal_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
    VkLayerDeviceCreateInfo* link = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo*)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    for (auto* cb = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext; cb; cb = (VkLayerDeviceCreateInfo*)cb->pNext)
        if (cb->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && cb->function == VK_LOADER_DATA_CALLBACK)
            setLoaderData = cb->u.pfnSetDeviceLoaderData;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    InstanceData* id = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_instances.find(Key(physicalDevice));
        if (it != g_instances.end()) id = it->second;
    }
    auto create = (PFN_vkCreateDevice)gipa(id ? id->instance : VK_NULL_HANDLE, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    auto* d = new DeviceData();
    d->device = *pDevice;
    d->phys = physicalDevice;
    d->inst = id;
    d->gdpa = gdpa;
    d->SetDeviceLoaderData = setLoaderData;
#define OAL_LOAD(name) d->name = (PFN_vk##name)gdpa(*pDevice, "vk" #name);
    OAL_DEVICE_FUNCS(OAL_LOAD)
#undef OAL_LOAD
    if (id && id->GetPhysicalDeviceMemoryProperties) id->GetPhysicalDeviceMemoryProperties(physicalDevice, &d->memProps);
    if (id && id->GetPhysicalDeviceQueueFamilyProperties)
    {
        uint32_t n = 0;
        id->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &n, nullptr);
        d->queueFamilies.resize(n);
        id->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &n, d->queueFamilies.data());
    }

    std::lock_guard<std::mutex> l(g_mutex);
    g_devices[Key(*pDevice)] = d;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL oal_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    if (!device) return;
    DeviceData* d = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_devices.find(Key(device));
        if (it == g_devices.end()) return;
        d = it->second;
        g_devices.erase(it);
        for (auto si = g_swaps.begin(); si != g_swaps.end();)
        {
            if (si->second->dev == d)
            {
                DestroySwapData(si->second);
                si = g_swaps.erase(si);
            }
            else ++si;
        }
    }
    d->DestroyDevice(device, pAllocator);
    delete d;
}

VKAPI_ATTR void VKAPI_CALL oal_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* pQueue)
{
    DeviceData* d;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        d = DeviceOf(Key(device));
    }
    d->GetDeviceQueue(device, family, index, pQueue);
    if (pQueue && *pQueue)
    {
        std::lock_guard<std::mutex> l(g_mutex);
        d->queueFamilyOf[*pQueue] = family;
    }
}

VKAPI_ATTR void VKAPI_CALL oal_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pInfo, VkQueue* pQueue)
{
    DeviceData* d;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        d = DeviceOf(Key(device));
    }
    d->GetDeviceQueue2(device, pInfo, pQueue);
    if (pInfo && pQueue && *pQueue)
    {
        std::lock_guard<std::mutex> l(g_mutex);
        d->queueFamilyOf[*pQueue] = pInfo->queueFamilyIndex;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL oal_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    DeviceData* d;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        d = DeviceOf(Key(device));
    }
    if (!Active()) return d->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

    VkSwapchainCreateInfoKHR ci = *pCreateInfo;
    bool modified = false;
    const char* why = "ok";
    VkSurfaceCapabilitiesKHR caps = {};
    if (!d->inst || !d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR ||
        d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, ci.surface, &caps) != VK_SUCCESS)
        why = "surface caps query failed";
    else if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        why = "surface does not allow TRANSFER_SRC";
    else if (!FormatHas(d, ci.imageFormat, VK_FORMAT_FEATURE_BLIT_SRC_BIT))
        why = "format cannot be a blit source";
    else
    {
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        modified = true;
    }

    VkResult res = d->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    if (res != VK_SUCCESS && modified)
    {
        Log("swapchain creation with TRANSFER_SRC failed (%d); retrying with the original settings", int(res));
        modified = false;
        why = "driver refused the extra usage flag";
        res = d->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }
    if (res != VK_SUCCESS) return res;

    std::lock_guard<std::mutex> l(g_mutex);
    auto* s = new SwapData();
    s->dev = d;
    s->swapchain = *pSwapchain;
    s->format = ci.imageFormat;
    s->colorSpace = ci.imageColorSpace;
    s->extent = ci.imageExtent;
    s->srcLayers = std::max(1u, ci.imageArrayLayers);
    s->layers = std::min(s->srcLayers, kMaxLayersCaptured);
    s->serial = ++g_serialCounter;
    s->linearFilter = FormatHas(d, ci.imageFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    for (uint32_t i = 0; i < kMaxSwapchainSlots; i++)
        if (!g_slotUsed[i])
        {
            g_slotUsed[i] = true;
            s->slot = i;
            break;
        }

    uint32_t count = 0;
    bool imagesOk = d->GetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr) == VK_SUCCESS && count > 0;
    if (imagesOk)
    {
        s->images.resize(count);
        imagesOk = d->GetSwapchainImagesKHR(device, *pSwapchain, &count, s->images.data()) == VK_SUCCESS;
    }
    s->semaphores.assign(s->images.size(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& sem : s->semaphores)
        if (d->CreateSemaphore(device, &sci, nullptr, &sem) != VK_SUCCESS) imagesOk = false;

    s->captureOk = modified && imagesOk && s->slot != UINT32_MAX;
    if (!imagesOk) why = "could not query swapchain images";
    else if (s->slot == UINT32_MAX && modified) why = "all capture slots in use";

    Log("swapchain %u created: %ux%u, %u layer(s), format %d, colorspace %d, usage 0x%x, %u images, slot %d -> capture %s (%s)",
        s->serial, ci.imageExtent.width, ci.imageExtent.height, s->srcLayers, int(ci.imageFormat),
        int(ci.imageColorSpace), unsigned(ci.imageUsage), unsigned(s->images.size()),
        s->slot == UINT32_MAX ? -1 : int(s->slot), s->captureOk ? "ON" : "OFF", why);

    g_swaps[*pSwapchain] = s;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL oal_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                   const VkAllocationCallbacks* pAllocator)
{
    DeviceData* d;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        d = DeviceOf(Key(device));
        auto it = g_swaps.find(swapchain);
        if (swapchain && it != g_swaps.end())
        {
            Log("swapchain %u destroyed", it->second->serial);
            DestroySwapData(it->second);
            g_swaps.erase(it);
        }
    }
    d->DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL oal_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    std::unique_lock<std::mutex> l(g_mutex);
    DeviceData* d = DeviceOf(Key(queue));
    if (!Active() || !pPresentInfo || pPresentInfo->swapchainCount == 0)
    {
        l.unlock();
        return d->QueuePresentKHR(queue, pPresentInfo);
    }

    const uint64_t now = NowNs();
    auto qf = d->queueFamilyOf.find(queue);
    const uint32_t family = qf == d->queueFamilyOf.end() ? UINT32_MAX : qf->second;

    // Collect finished readbacks for every swapchain in this present, then pick
    // at most one swapchain to capture (round robin) so a single submit with a
    // single fence covers it.
    SwapData* chosen = nullptr;
    RingSlot* slot = nullptr;
    uint32_t chosenIndex = 0;
    const uint32_t n = pPresentInfo->swapchainCount;
    for (uint32_t k = 0; k < n; k++)
    {
        uint32_t i = (g_roundRobin + k) % n;
        auto it = g_swaps.find(pPresentInfo->pSwapchains[i]);
        if (it == g_swaps.end()) continue;
        SwapData* s = it->second;
        if (!s->captureOk) continue;
        if (s->resourcesOk) slot = PollRing(s);
        if (chosen) continue;
        // 90% of the interval, so a 30 Hz target is not rounded up to every 4th frame at 90 Hz.
        if (now - s->lastCaptureNs < g_intervalNs / 10 * 9) continue;
        if (family == UINT32_MAX) continue;
        if (!s->resourcesTried)
        {
            if (!CreateCaptureResources(s, family))
            {
                DestroyCaptureResources(s);
                s->captureOk = false;
                continue;
            }
            slot = PollRing(s);
        }
        if (!s->resourcesOk || s->queueFamily != family) continue;
        if (!slot) continue; // all readbacks still in flight: skip, never wait
        uint32_t idx = pPresentInfo->pImageIndices[i];
        if (idx >= s->images.size()) continue;
        chosen = s;
        chosenIndex = idx;
        break;
    }
    g_roundRobin++;

    if (!chosen)
    {
        l.unlock();
        return d->QueuePresentKHR(queue, pPresentInfo);
    }

    RecordCapture(chosen, *slot, chosen->images[chosenIndex]);
    d->ResetFences(d->device, 1, &slot->fence);

    std::vector<VkPipelineStageFlags> stages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSemaphore signal = chosen->semaphores[chosenIndex];
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    si.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot->cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &signal;
    VkResult sr = d->QueueSubmit(queue, 1, &si, slot->fence);
    if (sr != VK_SUCCESS)
    {
        // A failed submit leaves the semaphores untouched, so the original
        // present is still valid. Stop capturing on this swapchain.
        Log("swapchain %u: capture submit failed (%d); capture off", chosen->serial, int(sr));
        chosen->captureOk = false;
        // Signal the fence we just reset with an empty submit so cleanup never waits on it.
        d->QueueSubmit(queue, 0, nullptr, slot->fence);
        l.unlock();
        return d->QueuePresentKHR(queue, pPresentInfo);
    }
    slot->pending = true;
    slot->timestampNs = now;
    chosen->lastCaptureNs = now;

    VkPresentInfoKHR pi = *pPresentInfo;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &signal;
    l.unlock();
    return d->QueuePresentKHR(queue, &pi);
}

PFN_vkVoidFunction DeviceIntercept(const char* name)
{
#define OAL_IF(fn) if (strcmp(name, "vk" #fn) == 0) return (PFN_vkVoidFunction)&oal_##fn;
    OAL_IF(DestroyDevice)
    if (!Active()) return nullptr;
    OAL_IF(GetDeviceQueue)
    OAL_IF(GetDeviceQueue2)
    OAL_IF(CreateSwapchainKHR)
    OAL_IF(DestroySwapchainKHR)
    OAL_IF(QueuePresentKHR)
#undef OAL_IF
    return nullptr;
}

} // namespace

OAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL oal_GetDeviceProcAddr(VkDevice device, const char* pName);

OAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL oal_GetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (!pName) return nullptr;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)&oal_GetInstanceProcAddr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)&oal_GetDeviceProcAddr;
    if (strcmp(pName, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)&oal_CreateInstance;
    if (strcmp(pName, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)&oal_DestroyInstance;
    if (strcmp(pName, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)&oal_CreateDevice;
    if (!instance) return nullptr;

    InstanceData* id = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        if (PFN_vkVoidFunction f = DeviceIntercept(pName)) return f;
        auto it = g_instances.find(Key(instance));
        if (it != g_instances.end()) id = it->second;
    }
    return id ? id->gipa(instance, pName) : nullptr;
}

OAL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL oal_GetDeviceProcAddr(VkDevice device, const char* pName)
{
    if (!pName || !device) return nullptr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)&oal_GetDeviceProcAddr;
    DeviceData* d = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        if (PFN_vkVoidFunction f = DeviceIntercept(pName)) return f;
        d = DeviceOf(Key(device));
    }
    return d ? d->gdpa(device, pName) : nullptr;
}
