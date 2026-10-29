/* See dc_libra.h. */
#include "dc_libra.h"

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#define LIBRA_RUNTIME_OPENGL
#define LIBRA_RUNTIME_VULKAN
#include "librashader_ld.h"

#include "desktop/host_main.h"
#include "desktop/opengl.h"
#include "desktop/sdl_compat.h"
#if SNESRECOMP_SDL3
#include <SDL3/SDL_vulkan.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ---------------------------------------------------------------- loader */

static libra_instance_t g_libra;
static int g_libra_state;            /* 0 untried, 1 loaded, -1 failed */
static char g_want[1100];            /* requested preset ("" = off) */
static bool g_want_dirty;
static int g_outcome;                /* see DcLibra_Poll */
static char g_outcome_msg[256];
static size_t g_frame_count;

static bool libra_load(void) {
  if (g_libra_state) return g_libra_state > 0;
#ifdef _WIN32
  /* librashader_ld.h asks for plain "librashader.dll". Load it first from the
   * game's own folders (the zip root ships it, and shaders/ beside a rebuilt
   * exe), so that name resolves to the module already in the process. */
  const char *base = SDL_GetBasePath();
  const char *const rel[] = {"librashader.dll", "shaders\\librashader.dll",
                             "..\\librashader.dll", "..\\shaders\\librashader.dll"};
  for (size_t i = 0; base && i < sizeof(rel) / sizeof(rel[0]); i++) {
    char p[1200];
    snprintf(p, sizeof(p), "%s%s", base, rel[i]);
    if (LoadLibraryA(p)) {
      fprintf(stderr, "[dc] librashader: %s\n", p);
      break;
    }
  }
#endif
  g_libra = librashader_load_instance();
  g_libra_state = g_libra.instance_loaded ? 1 : -1;
  if (g_libra_state < 0)
    fprintf(stderr, "[dc] librashader not found: slang presets unavailable\n");
  return g_libra_state > 0;
}

bool DcLibra_Available(void) { return libra_load(); }

static void libra_fail(const char *what, libra_error_t err) {
  char *msg = NULL;
  if (err && g_libra.error_write(err, &msg) == 0 && msg) {
    snprintf(g_outcome_msg, sizeof(g_outcome_msg), "%s: %s", what, msg);
    g_libra.error_free_string(&msg);
  } else {
    snprintf(g_outcome_msg, sizeof(g_outcome_msg), "%s", what);
  }
  if (err) g_libra.error_free(&err);
  fprintf(stderr, "[dc] librashader: %s\n", g_outcome_msg);
  g_outcome = -1;
}

void DcLibra_SetPreset(const char *path) {
  snprintf(g_want, sizeof(g_want), "%s", path ? path : "");
  g_want_dirty = true;
  g_outcome = 0;
}

const char *DcLibra_Preset(void) { return g_want[0] ? g_want : NULL; }

int DcLibra_Poll(char *msg, int cap) {
  int o = g_outcome;
  if (o && msg && cap > 0) snprintf(msg, (size_t)cap, "%s", g_outcome_msg);
  g_outcome = 0;
  return o;
}

static bool preset_open(libra_shader_preset_t *preset) {
  libra_error_t err = g_libra.preset_create(g_want, preset);
  if (err) {
    libra_fail("preset", err);
    return false;
  }
  return true;
}

/* ------------------------------------------------------- OpenGL presenter */

typedef unsigned int GLuint_;
typedef int GLint_;
#define DC_GL_TEXTURE_2D 0x0DE1
#define DC_GL_RGBA8 0x8058
#define DC_GL_RGBA 0x1908
#define DC_GL_UNSIGNED_BYTE 0x1401
#define DC_GL_FRAMEBUFFER 0x8D40
#define DC_GL_READ_FRAMEBUFFER 0x8CA8
#define DC_GL_DRAW_FRAMEBUFFER 0x8CA9
#define DC_GL_COLOR_ATTACHMENT0 0x8CE0
#define DC_GL_COLOR_BUFFER_BIT 0x4000
#define DC_GL_NEAREST 0x2600
#define DC_GL_LINEAR 0x2601
#define DC_GL_TEXTURE_MIN_FILTER 0x2801
#define DC_GL_TEXTURE_MAG_FILTER 0x2800
#define DC_GL_TEXTURE0 0x84C0
#define DC_GL_SCISSOR_TEST 0x0C11
#define DC_GL_BLEND 0x0BE2
#define DC_GL_TEXTURE_BASE_LEVEL 0x813C
#define DC_GL_TEXTURE_MAX_LEVEL 0x813D
#define DC_GL_FRAMEBUFFER_SRGB 0x8DB9
#define DC_APIENTRY SDLCALL

static struct {
  void (DC_APIENTRY *GenTextures)(int, GLuint_ *);
  void (DC_APIENTRY *DeleteTextures)(int, const GLuint_ *);
  void (DC_APIENTRY *BindTexture)(unsigned, GLuint_);
  void (DC_APIENTRY *TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
  void (DC_APIENTRY *TexParameteri)(unsigned, unsigned, int);
  void (DC_APIENTRY *GenFramebuffers)(int, GLuint_ *);
  void (DC_APIENTRY *DeleteFramebuffers)(int, const GLuint_ *);
  void (DC_APIENTRY *BindFramebuffer)(unsigned, GLuint_);
  void (DC_APIENTRY *FramebufferTexture2D)(unsigned, unsigned, unsigned, GLuint_, int);
  void (DC_APIENTRY *BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
  void (DC_APIENTRY *ActiveTexture)(unsigned);
  void (DC_APIENTRY *BindSampler)(unsigned, GLuint_);
  void (DC_APIENTRY *BindVertexArray)(GLuint_);
  void (DC_APIENTRY *UseProgram)(GLuint_);
  void (DC_APIENTRY *Viewport)(int, int, int, int);
  void (DC_APIENTRY *Disable)(unsigned);
} gl;

static const void *gl_loader(const char *name) {
  return (const void *)SDL_GL_GetProcAddress(name);
}

static bool gl_load(void) {
  static int state;
  if (state) return state > 0;
#define L(f) *(void **)&gl.f = (void *)SDL_GL_GetProcAddress("gl" #f); if (!gl.f) { state = -1; return false; }
  L(GenTextures) L(DeleteTextures) L(BindTexture) L(TexImage2D) L(TexParameteri)
  L(GenFramebuffers) L(DeleteFramebuffers) L(BindFramebuffer) L(FramebufferTexture2D)
  L(BlitFramebuffer) L(ActiveTexture) L(BindSampler) L(BindVertexArray) L(UseProgram)
  L(Viewport) L(Disable)
#undef L
  state = 1;
  return true;
}

static libra_gl_filter_chain_t g_gl_chain;
static char g_gl_loaded[1100];
static GLuint_ g_gl_out_tex, g_gl_fbo;
static int g_gl_out_w, g_gl_out_h;

static void gl_restore(int dw, int dh) {
  gl.BindFramebuffer(DC_GL_FRAMEBUFFER, 0);
  gl.ActiveTexture(DC_GL_TEXTURE0);
  for (unsigned u = 0; u < 16; u++) gl.BindSampler(u, 0);
  gl.BindVertexArray(0);
  gl.UseProgram(0);
  gl.Viewport(0, 0, dw, dh);
}

static bool gl_hook(unsigned texture, int tex_w, int tex_h, int vx, int vy,
                    int vw, int vh, int dw, int dh) {
  if (g_want_dirty || strcmp(g_want, g_gl_loaded) != 0) {
    g_want_dirty = false;
    if (g_gl_chain) {
      g_libra.gl_filter_chain_free(&g_gl_chain);
      g_gl_chain = NULL;
    }
    snprintf(g_gl_loaded, sizeof(g_gl_loaded), "%s", g_want);
    if (!g_want[0]) return false;
    if (!libra_load() || !gl_load()) {
      snprintf(g_outcome_msg, sizeof(g_outcome_msg), "librashader.dll not found");
      g_outcome = -1;
      g_gl_loaded[0] = g_want[0] = 0;
      return false;
    }
    libra_shader_preset_t preset = NULL;
    if (!preset_open(&preset)) {
      g_gl_loaded[0] = g_want[0] = 0;
      return false;
    }
    filter_chain_gl_opt_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.version = LIBRASHADER_CURRENT_VERSION;
    opt.glsl_version = 330;
    libra_error_t err = g_libra.gl_filter_chain_create(&preset, gl_loader, &opt, &g_gl_chain);
    if (preset) g_libra.preset_free(&preset);
    gl_restore(dw, dh);
    if (err) {
      g_gl_chain = NULL;
      libra_fail("OpenGL chain", err);
      g_gl_loaded[0] = g_want[0] = 0;
      return false;
    }
    g_frame_count = 0;
    snprintf(g_outcome_msg, sizeof(g_outcome_msg), "running (OpenGL)");
    g_outcome = 1;
  }
  if (!g_gl_chain || vw <= 0 || vh <= 0) return false;

  if (!g_gl_fbo || vw != g_gl_out_w || vh != g_gl_out_h) {
    if (!g_gl_fbo) {
      gl.GenFramebuffers(1, &g_gl_fbo);
      gl.GenTextures(1, &g_gl_out_tex);
    }
    gl.BindTexture(DC_GL_TEXTURE_2D, g_gl_out_tex);
    gl.TexImage2D(DC_GL_TEXTURE_2D, 0, DC_GL_RGBA8, vw, vh, 0, DC_GL_RGBA,
                  DC_GL_UNSIGNED_BYTE, NULL);
    gl.TexParameteri(DC_GL_TEXTURE_2D, DC_GL_TEXTURE_MIN_FILTER, DC_GL_LINEAR);
    gl.TexParameteri(DC_GL_TEXTURE_2D, DC_GL_TEXTURE_MAG_FILTER, DC_GL_LINEAR);
    gl.TexParameteri(DC_GL_TEXTURE_2D, DC_GL_TEXTURE_MAX_LEVEL, 0);
    gl.BindFramebuffer(DC_GL_FRAMEBUFFER, g_gl_fbo);
    gl.FramebufferTexture2D(DC_GL_FRAMEBUFFER, DC_GL_COLOR_ATTACHMENT0,
                            DC_GL_TEXTURE_2D, g_gl_out_tex, 0);
    gl.BindFramebuffer(DC_GL_FRAMEBUFFER, 0);
    g_gl_out_w = vw;
    g_gl_out_h = vh;
  }
  /* The presenter's texture is GL_RGBA holding the frame top row first. */
  libra_image_gl_t in = {texture, DC_GL_RGBA8, (uint32_t)tex_w, (uint32_t)tex_h};
  libra_image_gl_t out = {g_gl_out_tex, DC_GL_RGBA8, (uint32_t)vw, (uint32_t)vh};
  libra_viewport_t view = {0.0f, 0.0f, (uint32_t)vw, (uint32_t)vh};
  /* The host may leave scissor/blend on (overlays); the chain expects
   * neither. */
  gl.Disable(DC_GL_SCISSOR_TEST);
  gl.Disable(DC_GL_BLEND);
  /* The presenter's texture has no mip chain but keeps GL's default
   * MAX_LEVEL (1000); under a mipmapping sampler that makes it incomplete
   * and every sample reads (0,0,0,1). Pin it to level 0. */
  gl.BindTexture(DC_GL_TEXTURE_2D, texture);
  gl.TexParameteri(DC_GL_TEXTURE_2D, DC_GL_TEXTURE_BASE_LEVEL, 0);
  gl.TexParameteri(DC_GL_TEXTURE_2D, DC_GL_TEXTURE_MAX_LEVEL, 0);
  gl.BindTexture(DC_GL_TEXTURE_2D, 0);
  libra_error_t err = g_libra.gl_filter_chain_frame(&g_gl_chain, g_frame_count++, in,
                                                    out, &view, NULL, NULL);
  if (err) {
    libra_fail("OpenGL frame", err);
    g_libra.gl_filter_chain_free(&g_gl_chain);
    g_gl_chain = NULL;
    g_gl_loaded[0] = g_want[0] = 0;
    gl_restore(dw, dh);
    return false;
  }
  /* The chain keeps the input's row order, which is top-first: flip on the
   * way to the default framebuffer (bottom-left origin). */
  gl.Disable(DC_GL_SCISSOR_TEST);
  gl.BindFramebuffer(DC_GL_READ_FRAMEBUFFER, g_gl_fbo);
  gl.BindFramebuffer(DC_GL_DRAW_FRAMEBUFFER, 0);
  gl.BlitFramebuffer(0, 0, vw, vh, vx, vy + vh, vx + vw, vy,
                     DC_GL_COLOR_BUFFER_BIT, DC_GL_NEAREST);
  gl_restore(dw, dh);
  return true;
}

/* ----------------------------------------------------- SDL Vulkan renderer */

#if SNESRECOMP_SDL3
static struct {
  SDL_Renderer *renderer;
  bool ok;
  VkInstance instance;
  VkPhysicalDevice phys;
  VkDevice device;
  VkQueue queue;
  uint32_t family;
  PFN_vkGetInstanceProcAddr gipa;
  PFN_vkGetDeviceProcAddr gdpa;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
  PFN_vkGetDeviceQueue GetDeviceQueue;
  PFN_vkCreateCommandPool CreateCommandPool;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
  PFN_vkBeginCommandBuffer BeginCommandBuffer;
  PFN_vkEndCommandBuffer EndCommandBuffer;
  PFN_vkResetCommandBuffer ResetCommandBuffer;
  PFN_vkQueueSubmit QueueSubmit;
  PFN_vkQueueWaitIdle QueueWaitIdle;
  PFN_vkCreateFence CreateFence;
  PFN_vkWaitForFences WaitForFences;
  PFN_vkResetFences ResetFences;
  PFN_vkCreateBuffer CreateBuffer;
  PFN_vkDestroyBuffer DestroyBuffer;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
  PFN_vkBindBufferMemory BindBufferMemory;
  PFN_vkAllocateMemory AllocateMemory;
  PFN_vkFreeMemory FreeMemory;
  PFN_vkMapMemory MapMemory;
  PFN_vkCreateImage CreateImage;
  PFN_vkDestroyImage DestroyImage;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
  PFN_vkBindImageMemory BindImageMemory;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
  PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
  VkPhysicalDeviceMemoryProperties mem;
  VkCommandPool pool;
  VkCommandBuffer cb[2];
  VkFence fence;
  /* staging + input */
  VkBuffer stage;
  VkDeviceMemory stage_mem;
  void *stage_map;
  VkDeviceSize stage_size;
  VkImage in_img;
  VkDeviceMemory in_mem;
  int in_w, in_h;
  VkImageLayout in_layout;
  /* output, wrapped as an SDL texture */
  VkImage out_img;
  VkDeviceMemory out_mem;
  int out_w, out_h;
  SDL_Texture *out_tex;
  libra_vk_filter_chain_t chain;
  char loaded[1100];
} vk;

static bool vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out) {
  for (uint32_t i = 0; i < vk.mem.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (vk.mem.memoryTypes[i].propertyFlags & want) == want) {
      *out = i;
      return true;
    }
  return false;
}

static bool vk_init(SDL_Renderer *r) {
  if (vk.renderer == r) return vk.ok;
  memset(&vk, 0, sizeof(vk));
  vk.renderer = r;
  SDL_PropertiesID p = SDL_GetRendererProperties(r);
  vk.instance = (VkInstance)SDL_GetPointerProperty(p, SDL_PROP_RENDERER_VULKAN_INSTANCE_POINTER, NULL);
  vk.phys = (VkPhysicalDevice)SDL_GetPointerProperty(p, SDL_PROP_RENDERER_VULKAN_PHYSICAL_DEVICE_POINTER, NULL);
  vk.device = (VkDevice)SDL_GetPointerProperty(p, SDL_PROP_RENDERER_VULKAN_DEVICE_POINTER, NULL);
  vk.family = (uint32_t)SDL_GetNumberProperty(p, SDL_PROP_RENDERER_VULKAN_GRAPHICS_QUEUE_FAMILY_INDEX_NUMBER, 0);
  vk.gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
  if (!vk.instance || !vk.phys || !vk.device || !vk.gipa) {
    fprintf(stderr, "[dc] Vulkan renderer handles unavailable\n");
    return false;
  }
#define I(f) vk.f = (PFN_vk##f)vk.gipa(vk.instance, "vk" #f); if (!vk.f) return false;
  I(GetPhysicalDeviceMemoryProperties)
  vk.gdpa = (PFN_vkGetDeviceProcAddr)vk.gipa(vk.instance, "vkGetDeviceProcAddr");
  if (!vk.gdpa) return false;
#undef I
#define D(f) vk.f = (PFN_vk##f)vk.gdpa(vk.device, "vk" #f); if (!vk.f) return false;
  D(GetDeviceQueue) D(CreateCommandPool) D(AllocateCommandBuffers) D(BeginCommandBuffer)
  D(EndCommandBuffer) D(ResetCommandBuffer) D(QueueSubmit) D(QueueWaitIdle) D(CreateFence)
  D(WaitForFences) D(ResetFences) D(CreateBuffer) D(DestroyBuffer)
  D(GetBufferMemoryRequirements) D(BindBufferMemory) D(AllocateMemory) D(FreeMemory)
  D(MapMemory) D(CreateImage) D(DestroyImage) D(GetImageMemoryRequirements)
  D(BindImageMemory) D(CmdPipelineBarrier) D(CmdCopyBufferToImage)
#undef D
  vk.GetPhysicalDeviceMemoryProperties(vk.phys, &vk.mem);
  vk.GetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = vk.family;
  if (vk.CreateCommandPool(vk.device, &pci, NULL, &vk.pool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = vk.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 2;
  if (vk.AllocateCommandBuffers(vk.device, &cai, vk.cb) != VK_SUCCESS) return false;
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vk.CreateFence(vk.device, &fci, NULL, &vk.fence) != VK_SUCCESS) return false;
  vk.ok = true;
  fprintf(stderr, "[dc] Vulkan shader path ready (queue family %u)\n", vk.family);
  return true;
}

static bool vk_image(int w, int h, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem) {
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_B8G8R8A8_UNORM;
  ici.extent.width = (uint32_t)w;
  ici.extent.height = (uint32_t)h;
  ici.extent.depth = 1;
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk.CreateImage(vk.device, &ici, NULL, img) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vk.GetImageMemoryRequirements(vk.device, *img, &req);
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mai.memoryTypeIndex))
    return false;
  if (vk.AllocateMemory(vk.device, &mai, NULL, mem) != VK_SUCCESS) return false;
  return vk.BindImageMemory(vk.device, *img, *mem, 0) == VK_SUCCESS;
}

static void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags src_a, VkAccessFlags dst_a,
                       VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s) {
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src_a;
  b.dstAccessMask = dst_a;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.levelCount = 1;
  b.subresourceRange.layerCount = 1;
  vk.CmdPipelineBarrier(cb, src_s, dst_s, 0, 0, NULL, 0, NULL, 1, &b);
}

static bool vk_inputs(int w, int h) {
  VkDeviceSize need = (VkDeviceSize)w * h * 4;
  if (need > vk.stage_size) {
    if (vk.stage) {
      vk.DestroyBuffer(vk.device, vk.stage, NULL);
      vk.FreeMemory(vk.device, vk.stage_mem, NULL);
      vk.stage = VK_NULL_HANDLE;
    }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = need;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vk.CreateBuffer(vk.device, &bci, NULL, &vk.stage) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vk.GetBufferMemoryRequirements(vk.device, vk.stage, &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &mai.memoryTypeIndex))
      return false;
    if (vk.AllocateMemory(vk.device, &mai, NULL, &vk.stage_mem) != VK_SUCCESS) return false;
    vk.BindBufferMemory(vk.device, vk.stage, vk.stage_mem, 0);
    if (vk.MapMemory(vk.device, vk.stage_mem, 0, VK_WHOLE_SIZE, 0, &vk.stage_map) != VK_SUCCESS)
      return false;
    vk.stage_size = need;
  }
  if (w != vk.in_w || h != vk.in_h) {
    if (vk.in_img) {
      vk.DestroyImage(vk.device, vk.in_img, NULL);
      vk.FreeMemory(vk.device, vk.in_mem, NULL);
      vk.in_img = VK_NULL_HANDLE;
    }
    if (!vk_image(w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  &vk.in_img, &vk.in_mem))
      return false;
    vk.in_w = w;
    vk.in_h = h;
    vk.in_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  return true;
}

static bool vk_output(int w, int h) {
  if (vk.out_tex && w == vk.out_w && h == vk.out_h) return true;
  if (vk.out_tex) {
    SDL_DestroyTexture(vk.out_tex);
    vk.out_tex = NULL;
    vk.DestroyImage(vk.device, vk.out_img, NULL);
    vk.FreeMemory(vk.device, vk.out_mem, NULL);
  }
  if (!vk_image(w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                &vk.out_img, &vk.out_mem))
    return false;
  /* SDL samples it in SHADER_READ_ONLY_OPTIMAL; put it there before the
   * texture exists so SDL's assumption holds from the first use. */
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk.ResetCommandBuffer(vk.cb[0], 0);
  vk.BeginCommandBuffer(vk.cb[0], &bi);
  vk_barrier(vk.cb[0], vk.out_img, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  vk.EndCommandBuffer(vk.cb[0]);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &vk.cb[0];
  vk.ResetFences(vk.device, 1, &vk.fence);
  vk.QueueSubmit(vk.queue, 1, &si, vk.fence);
  vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, UINT64_MAX);
  SDL_PropertiesID p = SDL_CreateProperties();
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ARGB8888);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
  SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_VULKAN_TEXTURE_NUMBER, (Sint64)(uintptr_t)vk.out_img);
  vk.out_tex = SDL_CreateTextureWithProperties(vk.renderer, p);
  SDL_DestroyProperties(p);
  if (!vk.out_tex) {
    fprintf(stderr, "[dc] Vulkan: wrapping the shader output failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_SetTextureBlendMode(vk.out_tex, SDL_BLENDMODE_NONE);
  SDL_SetTextureScaleMode(vk.out_tex, SDL_SCALEMODE_NEAREST);
  vk.out_w = w;
  vk.out_h = h;
  return true;
}

static bool vk_hook(SDL_Renderer *r, const void *pixels, int pitch, int w, int h,
                    int pw, int ph, SDL_Texture **out) {
  bool switching = g_want_dirty || strcmp(g_want, vk.loaded) != 0;
  if (!switching && !vk.chain) return false;
  if (!vk_init(r)) {
    if (switching && g_want[0]) {
      snprintf(g_outcome_msg, sizeof(g_outcome_msg), "Vulkan renderer handles unavailable");
      g_outcome = -1;
      g_want[0] = 0;
    }
    g_want_dirty = false;
    return false;
  }
  if (switching) {
    g_want_dirty = false;
    vk.QueueWaitIdle(vk.queue);
    if (vk.chain) {
      g_libra.vk_filter_chain_free(&vk.chain);
      vk.chain = NULL;
    }
    snprintf(vk.loaded, sizeof(vk.loaded), "%s", g_want);
    if (!g_want[0]) return false;
    if (!libra_load()) {
      snprintf(g_outcome_msg, sizeof(g_outcome_msg), "librashader.dll not found");
      g_outcome = -1;
      vk.loaded[0] = g_want[0] = 0;
      return false;
    }
    libra_shader_preset_t preset = NULL;
    if (!preset_open(&preset)) {
      vk.loaded[0] = g_want[0] = 0;
      return false;
    }
    libra_device_vk_t dev = {vk.phys, vk.instance, vk.device, vk.queue, vk.gipa};
    filter_chain_vk_opt_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.version = LIBRASHADER_CURRENT_VERSION;
    opt.frames_in_flight = 2;
    libra_error_t err = g_libra.vk_filter_chain_create(&preset, dev, &opt, &vk.chain);
    if (preset) g_libra.preset_free(&preset);
    if (err) {
      vk.chain = NULL;
      libra_fail("Vulkan chain", err);
      vk.loaded[0] = g_want[0] = 0;
      return false;
    }
    g_frame_count = 0;
    snprintf(g_outcome_msg, sizeof(g_outcome_msg), "running (Vulkan)");
    g_outcome = 1;
  }
  if (w <= 0 || h <= 0 || pw <= 0 || ph <= 0) return false;
  /* SDL's own work from the previous frame may still read the output. */
  vk.QueueWaitIdle(vk.queue);
  if (!vk_inputs(w, h) || !vk_output(pw, ph)) return false;
  for (int y = 0; y < h; y++) {
    const uint32_t *src = (const uint32_t *)((const uint8_t *)pixels + (size_t)y * pitch);
    uint32_t *dst = (uint32_t *)vk.stage_map + (size_t)y * w;
    for (int x = 0; x < w; x++) dst[x] = src[x] | 0xFF000000u;  /* opaque */
  }
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  /* cb[0]: upload, and hand both images to the chain in its layouts. */
  vk.ResetCommandBuffer(vk.cb[0], 0);
  vk.BeginCommandBuffer(vk.cb[0], &bi);
  vk_barrier(vk.cb[0], vk.in_img, vk.in_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy region;
  memset(&region, 0, sizeof(region));
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent.width = (uint32_t)w;
  region.imageExtent.height = (uint32_t)h;
  region.imageExtent.depth = 1;
  vk.CmdCopyBufferToImage(vk.cb[0], vk.stage, vk.in_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          1, &region);
  vk_barrier(vk.cb[0], vk.in_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  vk.in_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vk_barrier(vk.cb[0], vk.out_img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  vk.EndCommandBuffer(vk.cb[0]);
  /* cb[1]: the chain (first command in its buffer), then back to sampling. */
  vk.ResetCommandBuffer(vk.cb[1], 0);
  vk.BeginCommandBuffer(vk.cb[1], &bi);
  libra_image_vk_t in = {vk.in_img, VK_FORMAT_B8G8R8A8_UNORM, (uint32_t)w, (uint32_t)h};
  libra_image_vk_t o = {vk.out_img, VK_FORMAT_B8G8R8A8_UNORM, (uint32_t)pw, (uint32_t)ph};
  libra_viewport_t view = {0.0f, 0.0f, (uint32_t)pw, (uint32_t)ph};
  libra_error_t err = g_libra.vk_filter_chain_frame(&vk.chain, vk.cb[1], g_frame_count++, in,
                                                    o, &view, NULL, NULL);
  vk_barrier(vk.cb[1], vk.out_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  vk.EndCommandBuffer(vk.cb[1]);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 2;
  si.pCommandBuffers = vk.cb;
  vk.ResetFences(vk.device, 1, &vk.fence);
  VkResult vr = vk.QueueSubmit(vk.queue, 1, &si, vk.fence);
  if (vr == VK_SUCCESS) vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, UINT64_MAX);
  if (err || vr != VK_SUCCESS) {
    if (err) libra_fail("Vulkan frame", err);
    vk.QueueWaitIdle(vk.queue);
    g_libra.vk_filter_chain_free(&vk.chain);
    vk.chain = NULL;
    vk.loaded[0] = g_want[0] = 0;
    return false;
  }
  *out = vk.out_tex;
  return true;
}
#endif /* SNESRECOMP_SDL3 */

/* --------------------------------------------------------------- public */

void DcLibra_Install(void) {
  static bool done;
  if (done) return;
  done = true;
  OpenGLRenderer_SetPostHook(gl_hook);
#if SNESRECOMP_SDL3
  snesrecomp_sdl_set_present_hook(vk_hook);
#endif
}

DcLibraBackend DcLibra_Backend(void) {
  if (OpenGLRenderer_IsActive()) return DC_LIBRA_GL;
#if SNESRECOMP_SDL3
  SDL_Renderer *r = snesrecomp_sdl_active_renderer();
  const char *name = r ? SDL_GetRendererName(r) : NULL;
  if (name && strcmp(name, "vulkan") == 0) return DC_LIBRA_VULKAN;
#endif
  return DC_LIBRA_NONE;
}
