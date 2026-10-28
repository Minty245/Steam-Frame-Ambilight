// Test harness (host only, not shipped): presents a known pattern through a
// headless swapchain so the capture layer can be exercised with a software
// Vulkan driver (lavapipe) and the Khronos validation layer.
//
// The pattern: left half red, right half blue, with a green band across the
// top 10% of the image. Optional: array layers (2) where layer 0 is all red and
// layer 1 all blue.
//
// usage: present_pattern [frames] [width] [height] [layers] [srgb]

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>

#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        VkResult r_ = (x);                                                                                             \
        if (r_ != VK_SUCCESS)                                                                                          \
        {                                                                                                              \
            fprintf(stderr, "%s failed: %d (line %d)\n", #x, int(r_), __LINE__);                                      \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

int main(int argc, char** argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 120;
    uint32_t W = argc > 2 ? uint32_t(atoi(argv[2])) : 1920;
    uint32_t H = argc > 3 ? uint32_t(atoi(argv[3])) : 960;
    uint32_t layers = argc > 4 ? uint32_t(atoi(argv[4])) : 1;
    bool srgb = argc > 5 && atoi(argv[5]) != 0;
    bool greyRight = argc > 6 && atoi(argv[6]) == 1; // right half mid-grey (128) instead of blue
    bool lensMode = argc > 6 && atoi(argv[6]) == 2;  // lens-shaped eye images (edge sampling test)
    VkFormat fmt = srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;

    const char* instExts[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExts;
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, nullptr, &inst));

    auto createHeadless = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
    VkHeadlessSurfaceCreateInfoEXT hci = {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface;
    CHECK(createHeadless(inst, &hci, nullptr, &surface));

    uint32_t n = 1;
    VkPhysicalDevice phys;
    vkEnumeratePhysicalDevices(inst, &n, &phys);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VkDevice dev;
    CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = caps.minImageCount;
    sci.imageFormat = fmt;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = {W, H};
    sci.imageArrayLayers = layers;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; // like a compositor that only writes
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR sc;
    CHECK(vkCreateSwapchainKHR(dev, &sci, nullptr, &sc));
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev, sc, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(dev, sc, &count, images.data());

    // Staging buffer with the pattern (BGRA bytes).
    VkDeviceSize bytes = VkDeviceSize(W) * H * 4 * layers;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, nullptr, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t type = 0;
    for (; type < mp.memoryTypeCount; type++)
        if ((mr.memoryTypeBits & (1u << type)) &&
            (mp.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            break;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    uint8_t* p;
    CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&p));
    for (uint32_t l = 0; l < layers; l++)
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++)
            {
                uint8_t* t = p + ((size_t(l) * H + y) * W + x) * 4;
                bool red = layers > 1 ? (l == 0) : (x < W / 2);
                bool green = layers == 1 && y < H / 10;
                t[0] = green ? 0 : (red ? 0 : 255);   // B
                t[1] = green ? 255 : 0;               // G
                t[2] = green ? 0 : (red ? 255 : 0);   // R
                if (greyRight && !red && !green) t[0] = t[1] = t[2] = 128;
                if (lensMode && layers == 1)
                {
                    // Lens-like eye images: black outside a circle per eye, green rim,
                    // red/blue interior, a white "menu" in the middle of each eye.
                    uint32_t half = W / 2, e = x / half;
                    double cx = e * half + half / 2.0, cy = H / 2.0, rad = H * 0.47, d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                    double dx = std::fabs(x + 0.5 - cx), dy = std::fabs(y + 0.5 - cy);
                    uint8_t R = 0, G = 0, B = 0;
                    if (d <= rad)
                    {
                        if (rad - d < rad * 0.12 && dx > rad * 0.5) G = 255;              // side rim
                        else if (dx < rad * 0.4 && dy < rad * 0.4) R = G = B = 255;      // menu
                        else if (e == 0) R = 255; else B = 255;
                    }
                    t[0] = B; t[1] = G; t[2] = R;
                }
                t[3] = 255;
            }

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    std::vector<VkCommandBuffer> cbs(count);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.commandBufferCount = count;
    CHECK(vkAllocateCommandBuffers(dev, &cai, cbs.data()));
    for (uint32_t i = 0; i < count; i++)
    {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cbs[i], &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[i];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cbs[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkBufferImageCopy c = {};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
        c.imageExtent = {W, H, 1};
        vkCmdCopyBufferToImage(cbs[i], buf, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cbs[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        vkEndCommandBuffer(cbs[i]);
    }

    VkSemaphoreCreateInfo semci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    std::vector<VkSemaphore> acquired(count), rendered(count);
    std::vector<VkFence> fences(count);
    for (uint32_t i = 0; i < count; i++)
    {
        vkCreateSemaphore(dev, &semci, nullptr, &acquired[i]);
        vkCreateSemaphore(dev, &semci, nullptr, &rendered[i]);
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &fci, nullptr, &fences[i]);
    }

    for (int f = 0; f < frames; f++)
    {
        uint32_t slot = uint32_t(f) % count;
        uint32_t idx;
        CHECK(vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired[slot], VK_NULL_HANDLE, &idx));
        // One fence per image: the command buffer for this image must be idle.
        vkWaitForFences(dev, 1, &fences[idx], VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fences[idx]);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired[slot];
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cbs[idx];
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered[slot];
        CHECK(vkQueueSubmit(queue, 1, &si, fences[idx]));
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered[slot];
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc;
        pi.pImageIndices = &idx;
        CHECK(vkQueuePresentKHR(queue, &pi));
        usleep(11000); // ~90 fps
    }
    vkDeviceWaitIdle(dev);

    for (uint32_t i = 0; i < count; i++)
    {
        vkDestroySemaphore(dev, acquired[i], nullptr);
        vkDestroySemaphore(dev, rendered[i], nullptr);
        vkDestroyFence(dev, fences[i], nullptr);
    }
    vkDestroyCommandPool(dev, pool, nullptr);
    vkUnmapMemory(dev, mem);
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    vkDestroySwapchainKHR(dev, sc, nullptr);
    vkDestroyDevice(dev, nullptr);
    auto destroySurface = (PFN_vkDestroySurfaceKHR)vkGetInstanceProcAddr(inst, "vkDestroySurfaceKHR");
    destroySurface(inst, surface, nullptr);
    vkDestroyInstance(inst, nullptr);
    printf("presented %d frames (%ux%u, %u layer(s), %s)\n", frames, W, H, layers, srgb ? "SRGB" : "UNORM");
    return 0;
}
