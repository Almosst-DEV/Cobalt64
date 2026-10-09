// test-vkdepth.c (bundle 10): checks the Vulkan call shapes the bundle's depth/stencil and MSAA code uses, on ANY Vulkan device (the host Mac: MoltenVK over the Apple GPU; no RADV is reachable here).
// Everything the bundle DECIDES comes from n48_depth.h, the same header Navi48Device.m uses: the Vulkan format and aspects of a depth format (n48dp_fmt), the barrier aspect (n48dp_barrier_aspect), the
// load / store operations of a depth attachment (n48rp_ds), the pipeline's samples and dynamic states (n48pc_config), the depth/stencil state (n48ds_key -> n48ds_decode) and the multisample
// resolve (vkCmdResolveImage after the pass).  So this proves the call parameters and the data movement, not RADV.
//   1. each depth/stencil format: an image, a render pass that CLEARS it (n48rp_ds), the clear value read back per aspect
//   2. depth-tested draws of overlapping rectangles: LESS with write, write OFF (must not write), all eight compare functions at a greater and an equal depth
//   3. the stencil operations table (S8_UINT): all eight operations, from values that tell clamp from wrap, plus a stencil-equal test that masks a draw
//   4. 4x MSAA: a diagonal edge rendered in 4 samples and resolved with vkCmdResolveImage: edge pixels are intermediate, the interior is exact
// build+run: clang -I/opt/homebrew/include test-vkdepth.c -L/opt/homebrew/lib -lvulkan -o /tmp/test-vkdepth && /tmp/test-vkdepth
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "n48_depth.h"
#include "test-depth-shaders.h"
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
static void freebuf(Buf *b) { vkUnmapMemory(dev, b->m); vkDestroyBuffer(dev, b->b, NULL); vkFreeMemory(dev, b->m, NULL); }
typedef struct { VkImage i; VkDeviceMemory m; VkImageView v; VkFormat f; unsigned aspects; VkImageLayout lay; uint32_t w, h; } Img;
static VkCommandBuffer begin(void) { VkCommandBuffer c; VkCommandBufferAllocateInfo a = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VK(vkAllocateCommandBuffers(dev, &a, &c)); VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; VK(vkBeginCommandBuffer(c, &bi)); return c; }
static void end(VkCommandBuffer c) { VK(vkEndCommandBuffer(c)); VkSubmitInfo s = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c }; VK(vkQueueSubmit(q, 1, &s, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q)); vkFreeCommandBuffers(dev, pool, 1, &c); }
// n48_tex_to: the barrier names the image's own aspects (n48dp_barrier_aspect)
static void to(VkCommandBuffer c, Img *im, VkImageLayout nl) {
    if (im->lay == nl) return;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = im->lay, .newLayout = nl, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im->i, .subresourceRange = { n48dp_barrier_aspect(im->aspects), 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b); im->lay = nl; }
static VkFormatFeatureFlags feats(VkFormat f) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, f, &fp); return fp.optimalTilingFeatures; }
static Img mkimg(VkFormat f, unsigned aspects, uint32_t w, uint32_t h, VkSampleCountFlagBits s, VkImageUsageFlags usage) {
    Img im = { .f = f, .aspects = aspects, .lay = VK_IMAGE_LAYOUT_UNDEFINED, .w = w, .h = h };
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = s, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(dev, &ic, NULL, &im.i)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im.i, &mr);
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) }; VK(vkAllocateMemory(dev, &ma, NULL, &im.m)); VK(vkBindImageMemory(dev, im.i, im.m, 0));
    // the ATTACHMENT view: every aspect of the format (n48AttViewLevel); a depth image's sampled view would carry n48dp_view_aspect only
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.i, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = { n48dp_barrier_aspect(aspects), 0, 1, 0, 1 } };
    VK(vkCreateImageView(dev, &vc, NULL, &im.v)); return im; }
static void freeimg(Img *im) { vkDestroyImageView(dev, im->v, NULL); vkDestroyImage(dev, im->i, NULL); vkFreeMemory(dev, im->m, NULL); }
// copy ONE aspect to a buffer (a combined format is copied aspect by aspect: n48dp_copy_aspect would refuse it, the test reads each aspect separately)
static void readback(Img *im, unsigned aspect, Buf *dst) {
    VkCommandBuffer c = begin(); to(c, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy b = { .imageSubresource = { aspect, 0, 0, 1 }, .imageExtent = { im->w, im->h, 1 } }; vkCmdCopyImageToBuffer(c, im->i, im->lay, dst->b, 1, &b); end(c); }

static unsigned mask_all(void) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p); return p.limits.framebufferColorSampleCounts & p.limits.framebufferDepthSampleCounts & p.limits.framebufferStencilSampleCounts; }
// A pass of one RGBA8 colour attachment (samples s) and an optional depth/stencil attachment.  The depth ops come from n48rp_ds, exactly as the bundle's render-pass setup.
typedef struct { VkRenderPass rp; VkFramebuffer fb; } Pass;
static Pass mkpass(Img *col, Img *dep, unsigned depthLoad, unsigned stencilLoad, VkSampleCountFlagBits s) {
    VkAttachmentDescription ad[2]; VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL }; uint32_t n = 0; VkImageView views[2]; uint32_t w = col ? col->w : dep->w, h = col ? col->h : dep->h;
    if (col) { ad[n] = (VkAttachmentDescription){ .format = col->f, .samples = s, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }; views[n] = col->v; n++; }   // every pass here CLEARS: it may start UNDEFINED
    if (dep) { n48rp_ds_t o; n48rp_ds(dep->aspects, 1, depthLoad, (dep->aspects & N48DP_ASP_STENCIL) != 0, stencilLoad, 1, &o); dref.attachment = n;
        ad[n] = (VkAttachmentDescription){ .format = dep->f, .samples = s, .loadOp = (VkAttachmentLoadOp)o.depthLoad, .storeOp = (VkAttachmentStoreOp)o.depthStore, .stencilLoadOp = (VkAttachmentLoadOp)o.stencilLoad, .stencilStoreOp = (VkAttachmentStoreOp)o.stencilStore,
            .initialLayout = o.undefinedInitial ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL }; views[n] = dep->v; n++; }
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = col ? 1 : 0, .pColorAttachments = col ? &cref : NULL, .pDepthStencilAttachment = dep ? &dref : NULL };
    VkRenderPassCreateInfo rpc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = n, .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sp }; Pass p; VK(vkCreateRenderPass(dev, &rpc, NULL, &p.rp));
    VkFramebufferCreateInfo fbc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = p.rp, .attachmentCount = n, .pAttachments = views, .width = w, .height = h, .layers = 1 }; VK(vkCreateFramebuffer(dev, &fbc, NULL, &p.fb)); return p; }
typedef struct { float rect[4], color[4], z, tri; } PC;
static VkPipelineLayout pl; static VkShaderModule vsm, fsm;
// A pipeline built the way the bundle builds one: n48pc_config for samples and dynamic states, n48ds_key -> n48ds_decode for the depth/stencil state.
static VkPipeline mkpipe(Pass *p, int hasD, int hasS, unsigned samples, unsigned long dcmp, int dwrite, const n48ds_face_in_t *f, const n48ds_face_in_t *b, int tri, n48pc_t *pcout) {
    (void)tri; n48pc_t pc; const char *why = NULL; if (!n48pc_config(hasD, hasS, samples, 0, mask_all(), &pc, &why)) { printf("FAIL n48pc_config: %s\n", why); exit(2); }
    n48ds_vk_t dv; n48ds_decode(n48ds_key(hasD, hasS, dcmp, dwrite, f, b), &dv);
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f, .depthBiasEnable = pc.depthBias ? VK_TRUE : VK_FALSE };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)pc.samples };
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = dv.depthTest ? VK_TRUE : VK_FALSE, .depthWriteEnable = dv.depthWrite ? VK_TRUE : VK_FALSE, .depthCompareOp = (VkCompareOp)dv.depthCompareOp,
        .stencilTestEnable = dv.stencilTest ? VK_TRUE : VK_FALSE,
        .front = { (VkStencilOp)dv.front.failOp, (VkStencilOp)dv.front.passOp, (VkStencilOp)dv.front.depthFailOp, (VkCompareOp)dv.front.compareOp, 0, 0, 0 },
        .back = { (VkStencilOp)dv.back.failOp, (VkStencilOp)dv.back.passOp, (VkStencilOp)dv.back.depthFailOp, (VkCompareOp)dv.back.compareOp, 0, 0, 0 } };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF }; VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[8]; for (unsigned i = 0; i < pc.ndyn; i++) dyn[i] = (VkDynamicState)pc.dyn[i];
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = pc.ndyn, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo g = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = pc.hasDS ? &dss : NULL, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = pl, .renderPass = p->rp };
    VkPipeline pp; VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &g, NULL, &pp)); if (pcout) *pcout = pc; return pp; }
// the bundle sets the stencil masks / references and the bias before every draw of a depth/stencil pipeline
static void setdyn(VkCommandBuffer c, const n48pc_t *pc, uint32_t sref, uint32_t rmask, uint32_t wmask) {
    for (unsigned i = 0; i < pc->ndyn; i++) switch (pc->dyn[i]) {
        case N48VK_DYN_STENCIL_COMPARE_MASK: vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, rmask); break;
        case N48VK_DYN_STENCIL_WRITE_MASK: vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, wmask); break;
        case N48VK_DYN_STENCIL_REFERENCE: vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, sref); break;
        case N48VK_DYN_DEPTH_BIAS: vkCmdSetDepthBias(c, 0.0f, 0.0f, 0.0f); break;
        default: break; } }
static void draw(VkCommandBuffer c, VkPipeline p, const n48pc_t *pc, uint32_t w, uint32_t h, float x0, float x1, float z, const float col[4], int tri, uint32_t sref) {
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkViewport vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f }; VkRect2D sc = { { 0, 0 }, { w, h } }; vkCmdSetViewport(c, 0, 1, &vp); vkCmdSetScissor(c, 0, 1, &sc); float bc[4] = { 0, 0, 0, 0 }; vkCmdSetBlendConstants(c, bc);
    setdyn(c, pc, sref, 0xFFFFFFFFu, 0xFFFFFFFFu);
    PC k = { { x0, 0.0f, x1, 1.0f }, { col[0], col[1], col[2], col[3] }, z, tri ? 1.0f : 0.0f }; vkCmdPushConstants(c, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof k, &k); vkCmdDraw(c, 4, 1, 0, 0); }

int main(void) {
    const char *iext[] = { "VK_KHR_portability_enumeration", "VK_KHR_get_physical_device_properties2" };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, .pApplicationInfo = &ai, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
    VkInstance inst; VK(vkCreateInstance(&ici, NULL, &inst)); uint32_t n = 1; (void)vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("device: %s, framebuffer sample counts color 0x%x depth 0x%x stencil 0x%x\n", pp.deviceName, pp.limits.framebufferColorSampleCounts, pp.limits.framebufferDepthSampleCounts, pp.limits.framebufferStencilSampleCounts);
    float pr = 1; VkDeviceQueueCreateInfo dq = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &pr };
    const char *dext[] = { "VK_KHR_portability_subset" }; uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL); VkExtensionProperties *ex = calloc(ne, sizeof *ex); vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);
    int sub = 0; for (uint32_t i = 0; i < ne; i++) if (!strcmp(ex[i].extensionName, "VK_KHR_portability_subset")) sub = 1;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq, .enabledExtensionCount = sub, .ppEnabledExtensionNames = dext };
    VK(vkCreateDevice(pd, &dci, NULL, &dev)); vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo pcc = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 }; VK(vkCreateCommandPool(dev, &pcc, NULL, &pool));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PC) }; VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr }; VK(vkCreatePipelineLayout(dev, &plc, NULL, &pl));
    VkShaderModuleCreateInfo sv = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof n48t_q_vert, .pCode = n48t_q_vert }, sf = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof n48t_q_frag, .pCode = n48t_q_frag };
    VK(vkCreateShaderModule(dev, &sv, NULL, &vsm)); VK(vkCreateShaderModule(dev, &sf, NULL, &fsm));
    const float RED[4] = { 1, 0, 0, 1 }, GREEN[4] = { 0, 1, 0, 1 }, BLUE[4] = { 0, 0, 1, 1 };

    // ---- 1. every depth/stencil format: create, clear through a render pass built from n48rp_ds, read the clear value back per aspect ----
    static const unsigned long pfs[4] = { N48DP_MTL_DEPTH16, N48DP_MTL_DEPTH32, N48DP_MTL_STENCIL8, N48DP_MTL_D32S8 };
    static const char *pfn[4] = { "Depth16Unorm", "Depth32Float", "Stencil8", "Depth32Float_Stencil8" };
    int usable[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < 4; k++) {
        n48dp_fmt_t df; CHECK(n48dp_fmt(pfs[k], &df), "%s: the bundle's table has it (vk %u, aspects 0x%x)", pfn[k], df.vk, df.aspects);
        VkFormatFeatureFlags ff = feats((VkFormat)df.vk); const char *why = NULL;
        int accepted = n48dp_check(N48DP_T_2D, N48DP_USE_RT | N48DP_USE_READ, N48DP_ST_PRIVATE, ff, &why);
        printf("note: %s vk %u optimal features 0x%x -> %s\n", pfn[k], df.vk, ff, accepted ? "accepted" : why);
        if (!accepted) continue;
        usable[k] = 1;
        VkImageUsageFlags iu = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | ((ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Img d = mkimg((VkFormat)df.vk, df.aspects, 8, 8, VK_SAMPLE_COUNT_1_BIT, iu);
        Pass p = mkpass(NULL, &d, N48LA_CLEAR, N48LA_CLEAR, VK_SAMPLE_COUNT_1_BIT);
        VkCommandBuffer c = begin(); VkClearValue cv = { .depthStencil = { 0.25f, 7 } }; VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p.rp, .framebuffer = p.fb, .renderArea = { { 0, 0 }, { 8, 8 } }, .clearValueCount = 1, .pClearValues = &cv };
        vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(c); end(c); d.lay = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (df.aspects & N48DP_ASP_DEPTH) {
            Buf b = mkbuf(8 * 8 * 4); memset(b.p, 0, b.n); readback(&d, N48DP_ASP_DEPTH, &b); int bad = 0;
            for (int i = 0; i < 64; i++) { if (df.vk == N48DP_VK_D16) { unsigned short v = ((unsigned short *)b.p)[i]; if (fabsf((float)v / 65535.0f - 0.25f) > 0.001f) bad++; } else { float v = ((float *)b.p)[i]; if (fabsf(v - 0.25f) > 0.0001f) bad++; } }
            CHECK(bad == 0, "%s: the DEPTH aspect cleared to 0.25 reads back exactly (%d of 64 texels differ)", pfn[k], bad); freebuf(&b); d.lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }
        if (df.aspects & N48DP_ASP_STENCIL) {
            Buf b = mkbuf(8 * 8); memset(b.p, 0, b.n); readback(&d, N48DP_ASP_STENCIL, &b); int bad = 0; for (int i = 0; i < 64; i++) if (((unsigned char *)b.p)[i] != 7) bad++;
            CHECK(bad == 0, "%s: the STENCIL aspect cleared to 7 reads back exactly (%d of 64 texels differ)", pfn[k], bad); freebuf(&b); }
        vkDestroyFramebuffer(dev, p.fb, NULL); vkDestroyRenderPass(dev, p.rp, NULL); freeimg(&d);
    }
    CHECK(usable[1], "Depth32Float is usable as a depth attachment on this device (the format the Safari / VectorKit studies need first)");

    // ---- 2. depth-tested draws: RGBA8 + D32_SFLOAT, 16x16 ----
    if (usable[1]) {
        enum { W = 16, H = 16 };
        Img col = mkimg(VK_FORMAT_R8G8B8A8_UNORM, N48DP_ASP_COLOR, W, H, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        Img dep = mkimg(VK_FORMAT_D32_SFLOAT, N48DP_ASP_DEPTH, W, H, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        Buf cb = mkbuf(W * H * 4), db = mkbuf(W * H * 4);
        // (a) LESS with write: red z=0.6 on x in [0,12), green z=0.3 on [4,16), blue z=0.9 everywhere (fails)
        { Pass p = mkpass(&col, &dep, N48LA_CLEAR, N48LA_CLEAR, VK_SAMPLE_COUNT_1_BIT); n48pc_t pc; VkPipeline pl1 = mkpipe(&p, 1, 0, 1, 1 /*Less*/, 1, &(n48ds_face_in_t){ 7, 0, 0, 0 }, &(n48ds_face_in_t){ 7, 0, 0, 0 }, 0, &pc);
          VkCommandBuffer c = begin(); VkClearValue cvs[2] = { { .color = { .float32 = { 0, 0, 0, 1 } } }, { .depthStencil = { 1.0f, 0 } } };
          VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p.rp, .framebuffer = p.fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 2, .pClearValues = cvs };
          vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE);
          draw(c, pl1, &pc, W, H, 0.0f, 0.75f, 0.6f, RED, 0, 0); draw(c, pl1, &pc, W, H, 0.25f, 1.0f, 0.3f, GREEN, 0, 0); draw(c, pl1, &pc, W, H, 0.0f, 1.0f, 0.9f, BLUE, 0, 0);
          vkCmdEndRenderPass(c); end(c); col.lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; dep.lay = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
          readback(&col, N48DP_ASP_COLOR, &cb); readback(&dep, N48DP_ASP_DEPTH, &db); col.lay = dep.lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          int badc = 0, badd = 0; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { const unsigned char *px = (unsigned char *)cb.p + (y * W + x) * 4; float dz = ((float *)db.p)[y * W + x];
              int wantRed = x < 4; if (wantRed ? !(px[0] == 255 && px[1] == 0 && px[2] == 0) : !(px[0] == 0 && px[1] == 255 && px[2] == 0)) badc++; if (fabsf(dz - (wantRed ? 0.6f : 0.3f)) > 0.001f) badd++; }
          CHECK(badc == 0, "depth LESS + write: the nearer green wins where it overlaps red (x 4..15), red stays where only red was drawn (x 0..3), the farther blue never shows (%d of %d pixels wrong)", badc, W * H);
          CHECK(badd == 0, "depth LESS + write: the depth buffer holds 0.6 where red won and 0.3 where green won (%d of %d texels wrong)", badd, W * H);
          vkDestroyPipeline(dev, pl1, NULL); vkDestroyFramebuffer(dev, p.fb, NULL); vkDestroyRenderPass(dev, p.rp, NULL); }
        // (b) depth write OFF: depth cleared 0.5, red z=0.2 then green z=0.4, LESS, no write: nothing was written, so BOTH pass and green (drawn last) is everywhere; the depth stays 0.5
        { Pass p = mkpass(&col, &dep, N48LA_CLEAR, N48LA_CLEAR, VK_SAMPLE_COUNT_1_BIT); n48pc_t pc; VkPipeline pl1 = mkpipe(&p, 1, 0, 1, 1, 0 /*no write*/, &(n48ds_face_in_t){ 7, 0, 0, 0 }, &(n48ds_face_in_t){ 7, 0, 0, 0 }, 0, &pc);
          VkCommandBuffer c = begin(); VkClearValue cvs[2] = { { .color = { .float32 = { 0, 0, 0, 1 } } }, { .depthStencil = { 0.5f, 0 } } };
          VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p.rp, .framebuffer = p.fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 2, .pClearValues = cvs };
          vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); draw(c, pl1, &pc, W, H, 0.0f, 1.0f, 0.2f, RED, 0, 0); draw(c, pl1, &pc, W, H, 0.0f, 1.0f, 0.4f, GREEN, 0, 0); vkCmdEndRenderPass(c); end(c);
          col.lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; dep.lay = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; readback(&col, N48DP_ASP_COLOR, &cb); readback(&dep, N48DP_ASP_DEPTH, &db); col.lay = dep.lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          int green = 0, kept = 0; for (int i = 0; i < W * H; i++) { const unsigned char *px = (unsigned char *)cb.p + i * 4; green += px[0] == 0 && px[1] == 255; kept += fabsf(((float *)db.p)[i] - 0.5f) < 0.0001f; }
          CHECK(green == W * H, "depth write OFF: the second (farther) draw is not rejected by the first, because the first wrote no depth (%d of %d pixels green)", green, W * H);
          CHECK(kept == W * H, "depth write OFF: the depth buffer keeps its 0.5 clear (%d of %d texels)", kept, W * H);
          vkDestroyPipeline(dev, pl1, NULL); vkDestroyFramebuffer(dev, p.fb, NULL); vkDestroyRenderPass(dev, p.rp, NULL); }
        // (c) all eight compare functions against a stored 0.5: a fragment at 0.7 and one at exactly 0.5
        { static const int passGreater[8] = { 0, 0, 0, 0, 1, 1, 1, 1 }, passEqual[8] = { 0, 0, 1, 1, 0, 0, 1, 1 }; const char *nm[8] = { "Never", "Less", "Equal", "LessEqual", "Greater", "NotEqual", "GreaterEqual", "Always" }; int badt = 0;
          for (unsigned long cf = 0; cf < 8; cf++) for (int pass = 0; pass < 2; pass++) {
              Pass p = mkpass(&col, &dep, N48LA_CLEAR, N48LA_CLEAR, VK_SAMPLE_COUNT_1_BIT); n48pc_t pc; VkPipeline pl1 = mkpipe(&p, 1, 0, 1, cf, 0, &(n48ds_face_in_t){ 7, 0, 0, 0 }, &(n48ds_face_in_t){ 7, 0, 0, 0 }, 0, &pc);
              VkCommandBuffer c = begin(); VkClearValue cvs[2] = { { .color = { .float32 = { 0, 0, 0, 1 } } }, { .depthStencil = { 0.5f, 0 } } };
              VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p.rp, .framebuffer = p.fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 2, .pClearValues = cvs };
              vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); draw(c, pl1, &pc, W, H, 0.0f, 1.0f, pass == 0 ? 0.7f : 0.5f, GREEN, 0, 0); vkCmdEndRenderPass(c); end(c);
              col.lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; dep.lay = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; readback(&col, N48DP_ASP_COLOR, &cb); col.lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; dep.lay = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
              int drawn = ((unsigned char *)cb.p)[1] == 255; int want = pass == 0 ? passGreater[cf] : passEqual[cf];
              if (drawn != want) { badt++; printf("  compare %s at %s depth: drawn %d wanted %d\n", nm[cf], pass == 0 ? "greater" : "equal", drawn, want); }
              vkDestroyPipeline(dev, pl1, NULL); vkDestroyFramebuffer(dev, p.fb, NULL); vkDestroyRenderPass(dev, p.rp, NULL); }
          CHECK(badt == 0, "all eight MTLCompareFunctions (Never .. Always) accept / reject a fragment at a greater and an equal depth as Metal defines them (%d of 16 cases wrong)", badt); }
        freebuf(&cb); freebuf(&db); freeimg(&col); freeimg(&dep);
    }

    // ---- 3. stencil operations (S8_UINT) and a stencil-equal mask ----
    if (usable[2]) {
        enum { W = 8, H = 8 };
        Img col = mkimg(VK_FORMAT_R8G8B8A8_UNORM, N48DP_ASP_COLOR, W, H, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        Img st = mkimg(VK_FORMAT_S8_UINT, N48DP_ASP_STENCIL, W, H, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        Buf sb = mkbuf(W * H);
        static const char *on[8] = { "Keep", "Zero", "Replace", "IncrementClamp", "DecrementClamp", "Invert", "IncrementWrap", "DecrementWrap" };
        static const int start[8] = { 5, 5, 5, 255, 0, 5, 255, 0 };   // IncrementClamp / IncrementWrap start at 255 and DecrementClamp / DecrementWrap at 0, so clamp and wrap differ
        static const int want[8] = { 5, 0, 9, 255, 0, 250, 0, 255 };
        int badops = 0;
        for (unsigned long op = 0; op < 8; op++) {
            Pass p = mkpass(&col, &st, N48LA_CLEAR, N48LA_CLEAR, VK_SAMPLE_COUNT_1_BIT); n48ds_face_in_t face = { 7 /*Always*/, 0, 0, op };   // pass operation = op
            n48pc_t pc; VkPipeline pl1 = mkpipe(&p, 0, 1, 1, 7, 0, &face, &face, 0, &pc);
            VkCommandBuffer c = begin(); VkClearValue cvs[2] = { { .color = { .float32 = { 0, 0, 0, 1 } } }, { .depthStencil = { 1.0f, (uint32_t)start[op] } } };
            VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p.rp, .framebuffer = p.fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 2, .pClearValues = cvs };
            vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); draw(c, pl1, &pc, W, H, 0.0f, 0.5f, 0.5f, GREEN, 0, 9); vkCmdEndRenderPass(c); end(c);
            col.lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; st.lay = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; readback(&st, N48DP_ASP_STENCIL, &sb); col.lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; st.lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            int left = ((unsigned char *)sb.p)[0], right = ((unsigned char *)sb.p)[W - 1];
            if (left != want[op] || right != start[op]) { badops++; printf("  stencil op %s from %d (ref 9): left half %d wanted %d, right half %d wanted %d\n", on[op], start[op], left, want[op], right, start[op]); }
            vkDestroyPipeline(dev, pl1, NULL); vkDestroyFramebuffer(dev, p.fb, NULL); vkDestroyRenderPass(dev, p.rp, NULL); }
        CHECK(badops == 0, "all eight stencil operations on S8_UINT (reference 9; clamp vs wrap told apart): the drawn half holds the operation's result, the undrawn half the clear value (%d of 8 wrong)", badops);
        // stencil EQUAL masks a draw: pass 1 writes ref 1 on x<4 (Replace); pass 2 draws green full-screen only where stencil == 1
        { Pass p = mkpass(&col, &st, N48LA_CLEAR, N48LA_CLEAR, VK_SAMPLE_COUNT_1_BIT); n48ds_face_in_t wr = { 7, 0, 0, 2 /*Replace*/ }, eq = { 2 /*Equal*/, 0, 0, 0 }; n48pc_t pcw, pce;
          VkPipeline pw = mkpipe(&p, 0, 1, 1, 7, 0, &wr, &wr, 0, &pcw), pe = mkpipe(&p, 0, 1, 1, 7, 0, &eq, &eq, 0, &pce);
          VkCommandBuffer c = begin(); VkClearValue cvs[2] = { { .color = { .float32 = { 0, 0, 0, 1 } } }, { .depthStencil = { 1.0f, 0 } } };
          VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p.rp, .framebuffer = p.fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 2, .pClearValues = cvs };
          vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); draw(c, pw, &pcw, W, H, 0.0f, 0.5f, 0.5f, RED, 0, 1); draw(c, pe, &pce, W, H, 0.0f, 1.0f, 0.5f, GREEN, 0, 1); vkCmdEndRenderPass(c); end(c);
          col.lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; Buf cb = mkbuf(W * H * 4); readback(&col, N48DP_ASP_COLOR, &cb); int bad = 0;
          for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { const unsigned char *px = (unsigned char *)cb.p + (y * W + x) * 4; int in = x < 4; if (in ? !(px[1] == 255) : !(px[0] == 0 && px[1] == 0)) bad++; }
          CHECK(bad == 0, "stencil EQUAL 1: the full-screen green draw appears only where the earlier draw replaced the stencil with 1 (x 0..3) (%d of %d pixels wrong; the red write pass has its colour overwritten by green)", bad, W * H);
          freebuf(&cb); vkDestroyPipeline(dev, pw, NULL); vkDestroyPipeline(dev, pe, NULL); vkDestroyFramebuffer(dev, p.fb, NULL); vkDestroyRenderPass(dev, p.rp, NULL); }
        freebuf(&sb); freeimg(&col); freeimg(&st);
    }

    // ---- 4. 4x MSAA colour, resolved into a 1x image with vkCmdResolveImage (the bundle's way) ----
    { unsigned m = mask_all(); CHECK(n48ms_device_supports(4, 1, m, m), "the device (framebuffer color/depth/stencil mask 0x%x) supports 4 samples through n48ms_device_supports", m);
      VkImageFormatProperties ifp; VkResult fr = vkGetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &ifp);
      CHECK(fr == VK_SUCCESS && n48ms_texture_ok(4, ifp.sampleCounts, m), "RGBA8 offers 4 samples (format counts 0x%x) through n48ms_texture_ok", fr == VK_SUCCESS ? ifp.sampleCounts : 0);
      if (fr == VK_SUCCESS && n48ms_texture_ok(4, ifp.sampleCounts, m)) {
        enum { W = 16, H = 16 };
        Img ms = mkimg(VK_FORMAT_R8G8B8A8_UNORM, N48DP_ASP_COLOR, W, H, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        Img rs = mkimg(VK_FORMAT_R8G8B8A8_UNORM, N48DP_ASP_COLOR, W, H, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        Pass p = mkpass(&ms, NULL, 0, 0, VK_SAMPLE_COUNT_4_BIT); n48pc_t pc; VkPipeline pl1 = mkpipe(&p, 0, 0, 4, 7, 0, &(n48ds_face_in_t){ 7, 0, 0, 0 }, &(n48ds_face_in_t){ 7, 0, 0, 0 }, 1, &pc);
        CHECK(pc.samples == 4 && pc.ndyn == 3, "the 4-sample colour-only pipeline config: 4 samples, the three old dynamic states");
        VkCommandBuffer c = begin(); VkClearValue cv = { .color = { .float32 = { 0, 0, 0, 1 } } };
        VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p.rp, .framebuffer = p.fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &cv };
        vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); draw(c, pl1, &pc, W, H, 0.0f, 1.0f, 0.5f, RED, 1 /*triangle*/, 0); vkCmdEndRenderPass(c);   // upper-left triangle, hypotenuse x + y = 16
        ms.lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to(c, &ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); to(c, &rs, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve rg = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { W, H, 1 } };
        vkCmdResolveImage(c, ms.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rs.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg); end(c);
        Buf b = mkbuf(W * H * 4); readback(&rs, N48DP_ASP_COLOR, &b);
        int inner = 0, outer = 0, edge = 0, inmid = 0; const unsigned char *px = b.p;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { int r = px[(y * W + x) * 4]; int s = x + y; if (s <= 13) { inner++; if (r == 255) inmid++; } else if (s >= 17) { outer++; } else if (s == 15) { edge++; } }
        int innerOk = inmid, outerOk = 0, edgeMid = 0; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { int r = px[(y * W + x) * 4], s = x + y; if (s >= 17 && r == 0) outerOk++; if (s == 15 && r > 10 && r < 245) edgeMid++; }
        printf("note: diagonal pixels (x+y == 15): R =");
        for (int y = 0; y < H; y++) printf(" %d", px[(y * W + (15 - y)) * 4]); printf("\n");
        CHECK(innerOk == inner, "MSAA 4x resolved: the interior (x+y <= 13) is exactly the triangle colour (%d of %d)", innerOk, inner);
        CHECK(outerOk == outer, "MSAA 4x resolved: the exterior (x+y >= 17) is exactly the clear colour (%d of %d)", outerOk, outer);
        CHECK(edgeMid >= edge - 2, "MSAA 4x resolved: the diagonal edge pixels hold INTERMEDIATE values (%d of %d), which a skipped resolve or a 1-sample render cannot produce", edgeMid, edge);
        freebuf(&b); vkDestroyPipeline(dev, pl1, NULL); vkDestroyFramebuffer(dev, p.fb, NULL); vkDestroyRenderPass(dev, p.rp, NULL); freeimg(&ms); freeimg(&rs); } }
    printf(fails ? "FAIL test-vkdepth (%d)\n" : "PASS test-vkdepth\n", fails); return fails ? 1 : 0;
}
