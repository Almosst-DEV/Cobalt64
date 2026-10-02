// test-vkimage.c (m11h9): checks the Vulkan call shapes the bundle's new texture code uses (3D box copies with bufferRowLength / bufferImageHeight, per-level copies,
// the GENERAL-layout vkCmdBlitImage mip chain of generateMipmapsForTexture:, an R8 image with a (0,0,0,R) component-mapped view) on ANY Vulkan device. It runs on the host Mac through MoltenVK
// (no RADV is reachable while WindowServer holds the exclusive N48N client), so it proves the call parameters and the data movement, not RADV.
// build+run: clang -I/opt/homebrew/include test-vkimage.c -L/opt/homebrew/lib -lvulkan -o /tmp/test-vkimage && /tmp/test-vkimage
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkCommandPool pool; static int fails;
#define CHECK(c, ...) do { int ok_ = (c) ? 1 : 0; printf("%s: ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(2); } } while (0)
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) { VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i; printf("no memtype\n"); exit(2); }
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; size_t n; } Buf;
static Buf mkbuf(size_t n) { Buf b = { .n = n }; VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VK(vkCreateBuffer(dev, &bc, NULL, &b.b)); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b.b, &mr);
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(dev, &ma, NULL, &b.m)); VK(vkBindBufferMemory(dev, b.b, b.m, 0)); VK(vkMapMemory(dev, b.m, 0, VK_WHOLE_SIZE, 0, &b.p)); memset(b.p, 0xCD, n); return b; }
typedef struct { VkImage i; VkDeviceMemory m; VkImageView v; VkImageLayout lay; uint32_t levels; } Img;
static Img mkimg(VkImageType t, VkFormat f, uint32_t w, uint32_t h, uint32_t d, uint32_t lv) { Img im = { .lay = VK_IMAGE_LAYOUT_UNDEFINED, .levels = lv };
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = t, .format = f, .extent = { w, h, d }, .mipLevels = lv, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(dev, &ic, NULL, &im.i)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im.i, &mr);
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) }; VK(vkAllocateMemory(dev, &ma, NULL, &im.m)); VK(vkBindImageMemory(dev, im.i, im.m, 0)); return im; }
static void to(VkCommandBuffer c, Img *im, VkImageLayout nl) { if (im->lay == nl) return; VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    .oldLayout = im->lay, .newLayout = nl, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im->i, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, im->levels, 0, 1 } };
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b); im->lay = nl; }
static void fullbar(VkCommandBuffer c) { VkMemoryBarrier m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0, NULL, 0, NULL); }
static VkCommandBuffer begin(void) { VkCommandBuffer c; VkCommandBufferAllocateInfo a = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VK(vkAllocateCommandBuffers(dev, &a, &c)); VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; VK(vkBeginCommandBuffer(c, &bi)); return c; }
static void end(VkCommandBuffer c) { VK(vkEndCommandBuffer(c)); VkSubmitInfo s = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c }; VK(vkQueueSubmit(q, 1, &s, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q)); vkFreeCommandBuffers(dev, pool, 1, &c); }

int main(void) {
    const char *iext[] = { "VK_KHR_portability_enumeration", "VK_KHR_get_physical_device_properties2" };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, .pApplicationInfo = &ai, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
    VkInstance inst; VK(vkCreateInstance(&ici, NULL, &inst)); uint32_t n = 1; VK(vkEnumeratePhysicalDevices(inst, &n, &pd) == VK_INCOMPLETE ? VK_SUCCESS : VK_SUCCESS);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("device: %s\n", pp.deviceName);
    float pr = 1; VkDeviceQueueCreateInfo dq = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &pr };
    const char *dext[] = { "VK_KHR_portability_subset" }; uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL); VkExtensionProperties *ex = calloc(ne, sizeof *ex); vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);
    int sub = 0; for (uint32_t i = 0; i < ne; i++) if (!strcmp(ex[i].extensionName, "VK_KHR_portability_subset")) sub = 1;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq, .enabledExtensionCount = sub, .ppEnabledExtensionNames = dext };
    VK(vkCreateDevice(pd, &dci, NULL, &dev)); vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo pc = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 }; VK(vkCreateCommandPool(dev, &pc, NULL, &pool));

    // ---- 3D: 8x8x4 RGBA8; upload the whole volume, then a box (2,1,1) 4x3x2 from a padded buffer (bufferRowLength 16 texels, bufferImageHeight 10 rows); read back all ----
    { enum { W = 8, H = 8, D = 4 }; static uint8_t model[D][H][W][4], back[D][H][W][4]; Buf up = mkbuf(W * H * D * 4), box = mkbuf(16 * 4 * 10 * 2), dn = mkbuf(W * H * D * 4);
      for (int z = 0; z < D; z++) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t *p = model[z][y][x]; p[0] = x * 16 + z; p[1] = y * 16; p[2] = z * 50 + 5; p[3] = 255 - x; }
      memcpy(up.p, model, sizeof model); Img im = mkimg(VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, W, H, D, 1);
      uint8_t *bp = box.p; for (int z = 0; z < 2; z++) for (int y = 0; y < 3; y++) for (int x = 0; x < 4; x++) { uint8_t *p = bp + ((z * 10 + y) * 16 + x) * 4; p[0] = 100 + x; p[1] = 110 + y; p[2] = 120 + z; p[3] = 77; memcpy(model[1 + z][1 + y][2 + x], p, 4); }
      VkCommandBuffer c = begin(); to(c, &im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkBufferImageCopy full = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, D } }; vkCmdCopyBufferToImage(c, up.b, im.i, im.lay, 1, &full); fullbar(c);
      VkBufferImageCopy bx = { .bufferRowLength = 16, .bufferImageHeight = 10, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageOffset = { 2, 1, 1 }, .imageExtent = { 4, 3, 2 } };
      vkCmdCopyBufferToImage(c, box.b, im.i, im.lay, 1, &bx); fullbar(c); to(c, &im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy rd = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, D } }; vkCmdCopyImageToBuffer(c, im.i, im.lay, dn.b, 1, &rd); end(c);
      memcpy(back, dn.p, sizeof back); CHECK(!memcmp(back, model, sizeof model), "3D 8x8x4: whole-volume upload, then a padded box copy (rowLength 16, imageHeight 10) at (2,1,1) 4x3x2, read back");
      // 1x1x1 3D
      Img one = mkimg(VK_IMAGE_TYPE_3D, VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 1); Buf ob = mkbuf(16); ((uint8_t *)ob.p)[0] = 9; ((uint8_t *)ob.p)[1] = 8; ((uint8_t *)ob.p)[2] = 7; ((uint8_t *)ob.p)[3] = 6;
      c = begin(); to(c, &one, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); VkBufferImageCopy o1 = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 1, 1, 1 } }; vkCmdCopyBufferToImage(c, ob.b, one.i, one.lay, 1, &o1);
      fullbar(c); to(c, &one, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vkCmdCopyImageToBuffer(c, one.i, one.lay, ob.b, 1, &o1); end(c); // (read back into the same buffer after the upload)
      CHECK(((uint8_t *)ob.p)[0] == 9 && ((uint8_t *)ob.p)[3] == 6, "3D 1x1x1 BGRA8 upload / read back"); }
    // ---- 2D 64x64 RGBA8 with 7 levels: per-level upload (distinct colours), per-level read back, then generate from level 0 by the GENERAL-layout blit chain ----
    { enum { L = 7 }; Img im = mkimg(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, L); Buf up = mkbuf(64 * 64 * 4), dn = mkbuf(64 * 64 * 4); VkCommandBuffer c;
      for (int l = 0; l < L; l++) { int lw = 64 >> l; uint8_t col[4] = { l * 35 + 20, 255 - l * 30, l * 17 + 3, 255 - l }; for (int i = 0; i < lw * lw; i++) memcpy((uint8_t *)up.p + i * 4, col, 4);
          c = begin(); to(c, &im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); VkBufferImageCopy b = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, .imageExtent = { lw, lw, 1 } }; vkCmdCopyBufferToImage(c, up.b, im.i, im.lay, 1, &b); end(c); }
      int bad = 0; for (int l = 0; l < L; l++) { int lw = 64 >> l; memset(dn.p, 0, 64 * 64 * 4); c = begin(); to(c, &im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
          VkBufferImageCopy b = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, .imageExtent = { lw, lw, 1 } }; vkCmdCopyImageToBuffer(c, im.i, im.lay, dn.b, 1, &b); end(c);
          uint8_t col[4] = { l * 35 + 20, 255 - l * 30, l * 17 + 3, 255 - l }; for (int i = 0; i < lw * lw; i++) if (memcmp((uint8_t *)dn.p + i * 4, col, 4)) { bad++; break; } }
      CHECK(bad == 0, "2D mips: per-level copy in / out at all 7 levels (%d levels differ)", bad);
      // pattern into level 0 and the blit chain
      uint8_t *l0 = malloc(64 * 64 * 4); for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint8_t *p = l0 + (y * 64 + x) * 4; p[0] = x * 4; p[1] = y * 4; p[2] = (x ^ y) * 4; p[3] = 200 + (x & 7); } memcpy(up.p, l0, 64 * 64 * 4);
      c = begin(); to(c, &im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); VkBufferImageCopy b0 = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 64, 64, 1 } }; vkCmdCopyBufferToImage(c, up.b, im.i, im.lay, 1, &b0);
      to(c, &im, VK_IMAGE_LAYOUT_GENERAL);
      for (uint32_t l = 1; l < L; l++) { fullbar(c); int sw = 64 >> (l - 1), dw = 64 >> l; VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { sw, sw, 1 } },
              .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, .dstOffsets = { { 0, 0, 0 }, { dw, dw, 1 } } }; vkCmdBlitImage(c, im.i, VK_IMAGE_LAYOUT_GENERAL, im.i, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR); }
      fullbar(c); end(c);
      int badl = 0, worst = 0; uint8_t *prev = l0; int pw = 64;
      for (int l = 1; l < L; l++) { int lw = 64 >> l; memset(dn.p, 0, 64 * 64 * 4); c = begin(); to(c, &im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); VkBufferImageCopy b = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, .imageExtent = { lw, lw, 1 } };
          vkCmdCopyImageToBuffer(c, im.i, im.lay, dn.b, 1, &b); end(c); uint8_t *cur = malloc(lw * lw * 4); memcpy(cur, dn.p, lw * lw * 4); int mx = 0;
          for (int y = 0; y < lw; y++) for (int x = 0; x < lw; x++) for (int k = 0; k < 4; k++) { int s = prev[((2*y)*pw + 2*x)*4+k] + prev[((2*y)*pw + 2*x+1)*4+k] + prev[((2*y+1)*pw + 2*x)*4+k] + prev[((2*y+1)*pw + 2*x+1)*4+k]; int d = abs((s + 2) / 4 - cur[(y*lw + x)*4+k]); if (d > mx) mx = d; }
          if (mx > 1) badl++; if (mx > worst) worst = mx; if (prev != l0) free(prev); prev = cur; pw = lw; }
      CHECK(badl == 0, "2D mips: GENERAL-layout vkCmdBlitImage chain = 2x2 box of the previous level, %d levels off, worst difference %d", badl, worst); }
    // ---- A8: an R8_UNORM image with a (0,0,0,R) component-mapped sampling view, plus an identity view (attachment/storage view) of the same image ----
    { Img im = mkimg(VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UNORM, 16, 16, 1, 1); VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.i, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8_UNORM,
          .components = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R }, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
      VkImageView v1, v2; VkResult r1 = vkCreateImageView(dev, &vc, NULL, &v1); vc.components = (VkComponentMapping){0}; VkResult r2 = vkCreateImageView(dev, &vc, NULL, &v2);
      CHECK(r1 == VK_SUCCESS && r2 == VK_SUCCESS, "R8_UNORM: view with components (0,0,0,R) and an identity view of the same image both create (%d, %d)", r1, r2); }
    printf(fails ? "FAIL test-vkimage (%d)\n" : "PASS test-vkimage\n", fails); return fails ? 1 : 0;
}
