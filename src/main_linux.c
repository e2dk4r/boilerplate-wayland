#include <liburing.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "content-type-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))
#define internal static
#define comptime static const
#define globalvar static
#define assert(x)                                                              \
  if (!(x)) {                                                                  \
    __builtin_debugtrap();                                                     \
  }

typedef __INT8_TYPE__ s8;
typedef __INT16_TYPE__ s16;
typedef __INT32_TYPE__ s32;
typedef __INT64_TYPE__ s64;

typedef __UINT8_TYPE__ u8;
typedef __UINT16_TYPE__ u16;
typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;

typedef u8 b8;

typedef float f32;
typedef double f64;

enum error_tag {
  ERROR_NONE,
  ERROR_MMAP,
  ERROR_OUTPUT_PTHREAD_CREATE,
  ERROR_WL_DISPLAY_CONNECT,
  ERROR_WL_DISPLAY_GET_REGISTRY,
  ERROR_WL_REGISTRY_GLOBAL,
  ERROR_WL_COMPOSITOR_CREATE_SURFACE,
  ERROR_XDG_WM_BASE_GET_XDG_SURFACE,
  ERROR_XDG_SURFACE_GET_TOPLEVEL,
  ERROR_MEMFD_CREATE_WL_SHM,
  ERROR_FTRUNCATE_WL_SHM,
  ERROR_MMAP_WL_SHM,
  ERROR_WL_SHM_CREATE_POOL,
  ERROR_IO_URING_QUEUE_INIT,
  ERROR_IO_URING_WAIT_CQE,
  ERROR_XKB_CONTEXT_NEW,
};

struct memory_arena {
  u64 size;
  u64 used;
  u8 *data;
};

internal b8 IsPowerOfTwo(u64 value) {
  return value != 0 && (value & (value - 1)) == 0;
}

internal u8 *MemoryArenaPush(struct memory_arena *arena, u64 size,
                             u64 alignment) {
  assert(IsPowerOfTwo(alignment));
  u64 alignOffset = 0;
  {
    u64 alignMask = alignment - 1;
    u64 alignResult = (u64)(arena->data + arena->used) & alignMask;
    b8 isDataNotAligned = alignResult != 0;
    if (isDataNotAligned) {
      alignOffset = alignment - alignResult;
    }
  }
  size += alignOffset;

  assert(arena->used + size <= arena->size);
  u8 *block = arena->data + arena->used + alignOffset;
  arena->used += size;

  return block;
}

internal struct memory_arena MemoryArenaSub(struct memory_arena *master,
                                            u64 size) {
  assert(master->used + size <= master->size);

  struct memory_arena sub = {
      .size = size,
      .data = master->data + master->used,
  };

  master->used += size;
  return sub;
}

/*
 * Example usage
 *
 * struct memory_arena *arena = ...;
 * struct memory_temp tempTextMemory = MemoryTempBegin(arena);
 * ...
 * MemoryTempEnd(&tempTextMemory);
 *
 * */
struct memory_temp {
  struct memory_arena *arena;
  u64 usedInTheBeginning;
};

internal struct memory_temp MemoryTempBegin(struct memory_arena *arena) {
  return (struct memory_temp){
      .arena = arena,
      .usedInTheBeginning = arena->used,
  };
}

internal void MemoryTempEnd(struct memory_temp *temp) {
  struct memory_arena *arena = temp->arena;
  arena->used = temp->usedInTheBeginning;
}

struct framebuffer {
  u16 width;
  u16 height;
  u16 stride;
  u8 *data;
};

internal void DrawSolid(struct framebuffer *framebuffer, u32 color) {
  u16 width = framebuffer->width;
  u16 height = framebuffer->height;
  u16 stride = framebuffer->stride;
  u8 *row = framebuffer->data;

  for (u16 y = 0; y < height; y++) {
    u32 *pixel = (u32 *)row;
    for (u16 x = 0; x < width; x++) {
      *pixel = color;
      pixel++;
    }
    row += stride;
  }
}

internal void DrawCheckerBoard(struct framebuffer *framebuffer, u32 lightColor,
                               u32 darkColor, f32 offset) {
  u16 width = framebuffer->width;
  u16 height = framebuffer->height;
  u16 stride = framebuffer->stride;
  u8 *row = framebuffer->data;

  u16 checkerSizeInPixels = 350;

  for (u16 y = 0; y < height; y++) {
    u32 *pixel = (u32 *)row;
    for (u16 x = (u16)(offset * 10.0f); x < width; x++) {
      if (((y / checkerSizeInPixels) & 1) ^ ((x / checkerSizeInPixels) & 1))
        *pixel = lightColor;
      else
        *pixel = darkColor;

      pixel++;
    }
    row += stride;
  }
}

struct button {
  b8 isPressed : 1;
};

struct input {
  struct button up;
  struct button down;
  struct button left;
  struct button right;
};

struct input *InputGetKeyboardAndMouse(struct input *inputs, u32 inputCount) {
  assert(inputCount != 0);
  struct input *keyboardAndMouseInput = inputs + 0;
  return keyboardAndMouseInput;
}

struct wayland_present {
  b8 isSynced : 1;
  u64 ust;
  u64 refresh;
};

struct linux_context {
  // memory
  struct memory_arena memoryArena;
  struct memory_arena framebufferArena;
  struct memory_arena xkbArena;

  // image
  struct framebuffer framebuffer;

  // wayland globals
  struct wl_display *wl_display;
  struct wl_registry *wl_registry;
  struct wl_compositor *wl_compositor;
  struct wl_shm *wl_shm;
  struct xdg_wm_base *xdg_wm_base;
  struct wl_seat *wl_seat;
  struct wp_presentation *wp_presentation;
  struct wp_content_type_manager_v1 *wp_content_type_manager_v1;

  // wayland objects
  struct wl_surface *wl_surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;
  struct wl_buffer *wl_buffer;
  struct wl_keyboard *wl_keyboard;
  struct wl_pointer *wl_pointer;

  // xkb
  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;

  struct wayland_present present;

  struct input inputs[2];

  f32 offset;
};

internal void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                               uint32_t serial, struct wl_surface *surface,
                               wl_fixed_t surface_x, wl_fixed_t surface_y) {}

internal void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                               uint32_t serial, struct wl_surface *surface) {}

internal void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
                                uint32_t time, wl_fixed_t surface_x,
                                wl_fixed_t surface_y) {}

internal void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                                uint32_t serial, uint32_t time, uint32_t button,
                                uint32_t state) {}

internal void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                              uint32_t time, uint32_t axis, wl_fixed_t value) {}

internal void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {}

comptime struct wl_pointer_listener wl_pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
    .axis = wl_pointer_axis,
    .frame = wl_pointer_frame,
};

internal void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                                 uint32_t format, int32_t fd, uint32_t size) {
  assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

  struct linux_context *context = data;

  // TODO: manage memory of xkb_keymap allocations
  struct memory_temp xkbKeymapMemory = MemoryTempBegin(&context->xkbArena);
  char *keymapString = (char *)MemoryArenaPush(xkbKeymapMemory.arena, size, 4);
  {
    void *mmapResult =
        mmap(keymapString, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    b8 isKeymapReadFromFile = mmapResult != MAP_FAILED;
    assert(isKeymapReadFromFile);

    close(fd);
  }

  struct xkb_keymap *xkb_keymap = xkb_keymap_new_from_buffer(
      context->xkb_context, keymapString, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!xkb_keymap) {
    // TODO: handle xkb_keymap error
    MemoryTempEnd(&xkbKeymapMemory);
    return;
  }

  struct xkb_state *xkb_state = xkb_state_new(xkb_keymap);
  if (!xkb_state) {
    // TODO: handle xkb_state error
    xkb_keymap_unref(xkb_keymap);
    MemoryTempEnd(&xkbKeymapMemory);
    return;
  }

  xkb_keymap_unref(context->xkb_keymap);
  xkb_state_unref(context->xkb_state);

  context->xkb_keymap = xkb_keymap;
  context->xkb_state = xkb_state;
}

internal void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
                                uint32_t serial, struct wl_surface *surface,
                                struct wl_array *keys) {}

internal void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
                                uint32_t serial, struct wl_surface *surface) {}

internal void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
                              uint32_t serial, uint32_t time, uint32_t key,
                              uint32_t state) {
  struct linux_context *context = data;
  struct input *keyboardAndMouseInput =
      InputGetKeyboardAndMouse(context->inputs, ARRAY_SIZE(context->inputs));

  assert(context->xkb_state);

  // see: WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1
  key += 8;

  xkb_keysym_t keysym = xkb_state_key_get_one_sym(context->xkb_state, key);

  b8 isPressed = state != WL_KEYBOARD_KEY_STATE_RELEASED;

  switch (keysym) {
  case XKB_KEY_a: {
    keyboardAndMouseInput->left.isPressed = isPressed;
  } break;

  case XKB_KEY_d: {
    keyboardAndMouseInput->right.isPressed = isPressed;
  } break;

  case XKB_KEY_w: {
    keyboardAndMouseInput->up.isPressed = isPressed;
  } break;

  case XKB_KEY_s: {
    keyboardAndMouseInput->down.isPressed = isPressed;
  } break;

  case XKB_KEY_q: {
    keyboardAndMouseInput->down.isPressed = isPressed;
  } break;
  }

  // printf("state %d up: %d down: %d left: %d right: %d\n", isPressed,
  //        keyboardAndMouseInput->up.isPressed,
  //        keyboardAndMouseInput->down.isPressed,
  //        keyboardAndMouseInput->left.isPressed,
  //        keyboardAndMouseInput->right.isPressed);
}

internal void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                                    uint32_t serial, uint32_t mods_depressed,
                                    uint32_t mods_latched, uint32_t mods_locked,
                                    uint32_t group) {
  struct linux_context *context = data;
  xkb_state_update_mask(context->xkb_state, mods_depressed, mods_latched,
                        mods_locked, 0, 0, group);
}

internal void wl_keyboard_repeat_info(void *data,
                                      struct wl_keyboard *wl_keyboard,
                                      int32_t rate, int32_t delay) {}

comptime struct wl_keyboard_listener wl_keyboard_listener = {
    .keymap = wl_keyboard_keymap,
    .enter = wl_keyboard_enter,
    .leave = wl_keyboard_leave,
    .key = wl_keyboard_key,
    .modifiers = wl_keyboard_modifiers,
    .repeat_info = wl_keyboard_repeat_info,
};

internal void wl_seat_capabilities(void *data, struct wl_seat *wl_seat,
                                   uint32_t capabilities) {
  struct linux_context *context = data;

  // - keyboard
  b8 haveKeyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
  if (haveKeyboard && !context->wl_keyboard) {
    context->wl_keyboard = wl_seat_get_keyboard(wl_seat);
    wl_keyboard_add_listener(context->wl_keyboard, &wl_keyboard_listener,
                             context);
  } else if (!haveKeyboard && context->wl_keyboard) {
    wl_keyboard_release(context->wl_keyboard);
    context->wl_keyboard = 0;
  }

  // - pointer
  b8 havePointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
  if (havePointer && !context->wl_pointer) {
    context->wl_pointer = wl_seat_get_pointer(wl_seat);
    wl_pointer_add_listener(context->wl_pointer, &wl_pointer_listener, context);
  } else if (!havePointer && context->wl_pointer) {
    wl_pointer_release(context->wl_pointer);
    context->wl_pointer = 0;
  }
}

internal void wl_seat_name(void *data, struct wl_seat *wl_seat,
                           const char *name) {}

comptime struct wl_seat_listener wl_seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
};

internal void wp_presentation_feedback_presented(
    void *data, struct wp_presentation_feedback *wp_presentation_feedback,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh,
    uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
  struct linux_context *context = data;

  // - destroy this callback
  wp_presentation_feedback_destroy(wp_presentation_feedback);

  struct wayland_present *present = &context->present;
  u64 nanosecondsPerSecond = 1000000000 /* 1e9 */;
  present->ust =
      ((u64)tv_sec_hi << 32 | tv_sec_lo) * nanosecondsPerSecond + tv_nsec;
  present->refresh = refresh;
}

internal void wp_presentation_feedback_discarded(
    void *data, struct wp_presentation_feedback *wp_presentation_feedback) {}

comptime struct wp_presentation_feedback_listener
    wp_presentation_feedback_listener = {
        .presented = wp_presentation_feedback_presented,
        .discarded = wp_presentation_feedback_discarded,
};

comptime struct wl_callback_listener wl_surface_frame_listener;
internal void
wl_surface_frame_done(void *data, struct wl_callback *wl_surface_frame_callback,
                      uint32_t nowInMilliseconds) {
  struct linux_context *context = data;

  // - destroy this callback
  wl_callback_destroy(wl_surface_frame_callback);

  // - request another frame
  wl_surface_frame_callback = wl_surface_frame(context->wl_surface);
  wl_callback_add_listener(wl_surface_frame_callback,
                           &wl_surface_frame_listener, context);

  // - presentation feedback
  struct wp_presentation_feedback *feedback =
      wp_presentation_feedback(context->wp_presentation, context->wl_surface);
  wp_presentation_feedback_add_listener(
      feedback, &wp_presentation_feedback_listener, context);

  /*
  // update
  globalvar u32 lastFrameAt = 0;
  if (lastFrameAt == 0) {
    lastFrameAt = nowInMilliseconds;
  }

  u32 elapsed = nowInMilliseconds - lastFrameAt;
  u32 millisecondsPerFrame = (u32)(33.333f);
  if (elapsed < millisecondsPerFrame) {
    wl_surface_commit(context->wl_surface);
    return;
  }
  lastFrameAt = nowInMilliseconds;

  f32 speed = 5.0f;
  f32 deltaTime = (f32)elapsed / 1e3f;
  context->offset += deltaTime * speed;

  DrawCheckerBoard(&context->framebuffer, 0xcbd5e1, 0x0f172a, context->offset);
  wl_surface_attach(context->wl_surface, context->wl_buffer, 0, 0);
  wl_surface_damage_buffer(context->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(context->wl_surface);
  */
}

comptime struct wl_callback_listener wl_surface_frame_listener = {
    .done = wl_surface_frame_done,
};

internal void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                    uint32_t serial) {
  xdg_surface_ack_configure(xdg_surface, serial);

  struct linux_context *context = data;

  wl_surface_attach(context->wl_surface, context->wl_buffer, 0, 0);
  wl_surface_commit(context->wl_surface);
}

comptime struct xdg_surface_listener xdg_surface_listener = {

    .configure = xdg_surface_configure,
};

internal void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                               uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
}

comptime struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

internal void wl_registry_global(void *data, struct wl_registry *wl_registry,
                                 uint32_t name, const char *interface,
                                 uint32_t version) {
  struct linux_context *context = data;
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    context->wl_compositor =
        wl_registry_bind(wl_registry, name, &wl_compositor_interface, version);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    context->wl_shm =
        wl_registry_bind(wl_registry, name, &wl_shm_interface, version);
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    context->xdg_wm_base =
        wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, version);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    context->wl_seat =
        wl_registry_bind(wl_registry, name, &wl_seat_interface, version);
  } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
    context->wp_presentation = wl_registry_bind(
        wl_registry, name, &wp_presentation_interface, version);
  } else if (strcmp(interface, wp_content_type_manager_v1_interface.name) ==
             0) {
    context->wp_content_type_manager_v1 = wl_registry_bind(
        wl_registry, name, &wp_content_type_manager_v1_interface, version);
  }
}

comptime struct wl_registry_listener wl_registry_listener = {
    .global = wl_registry_global,
};

internal u64 Now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((u64)ts.tv_sec * 1000000000 /* 1e9 */) + (u64)ts.tv_nsec;
}

void *OutputThreadMain(void *threadData) {
  return 0;
  struct linux_context *context = threadData;

  // event loop
  struct op_timer {
    struct __kernel_timespec ts;
  };

  struct io_uring ring;
  if (io_uring_queue_init(1, &ring, 0) != 0) {
    // errorTag = ERROR_IO_URING_QUEUE_INIT;
    return 0;
  }

  // - timer
  struct op_timer timerOp = {};
  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    timerOp.ts.tv_nsec = 33333333; // 33.333333ms, 1ms = 1e6 ns

    // one time request
    // io_uring_prep_timeout(sqe, &timerOp.ts, 1, 0);

    // infinite timers at every ts
    io_uring_prep_timeout(sqe, &timerOp.ts, 0, IORING_TIMEOUT_MULTISHOT);

    io_uring_sqe_set_data(sqe, &timerOp);
  }

  io_uring_submit(&ring);

  struct io_uring_cqe *cqe;
  u64 previousFrame = Now();
  while (1) {
  wait_cqe:
    if (io_uring_wait_cqe(&ring, &cqe) != 0) {
      if (errno == EAGAIN)
        goto wait_cqe;

      // errorTag = ERROR_IO_URING_WAIT_CQE;
      break;
    }

    void *data = io_uring_cqe_get_data(cqe);
    if (data == &timerOp) {
      u64 now = Now();
      u64 elapsed = now - previousFrame;

      // printf("%lu => %luns elapsed\n", now, elapsed);
      f32 deltaTime = (f32)elapsed / 1e9f;
      f32 speed = 5.0f;

      context->offset += deltaTime * speed;
      // context.offset += 0.33f;
      DrawCheckerBoard(&context->framebuffer, 0xcbd5e1, 0x0f172a,
                       context->offset);
      wl_surface_attach(context->wl_surface, context->wl_buffer, 0, 0);
      wl_surface_damage_buffer(context->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
      wl_surface_commit(context->wl_surface);

      previousFrame = now;
    }

    io_uring_cqe_seen(&ring, cqe);
  }

  return 0;
}

int main(int argc, char *argv[]) {
  struct linux_context context = {};
  enum error_tag errorTag = ERROR_NONE;

  // memory
  struct memory_arena *memoryArena = &context.memoryArena;
  {
    u64 MEGABYTES = 1 << 20;
    // TODO: tweak this after release
    u64 totalMemoryAllocated = 64 * MEGABYTES;

    // TODO: maybe MAP_UNINITIALIZED?
    *memoryArena = (struct memory_arena){
        .size = totalMemoryAllocated,
        .data = mmap(0, totalMemoryAllocated, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
    };
    if (memoryArena->data == MAP_FAILED) {
      errorTag = ERROR_MMAP;
      goto exit;
    }

    // TODO: tweak this
    // 1920x1080x4 = ~7.91m
    context.framebufferArena = MemoryArenaSub(memoryArena, 8 * MEGABYTES);

    // TODO: tweak this
    context.xkbArena = MemoryArenaSub(memoryArena, 1 * MEGABYTES);
  }

  // framebuffer
  struct framebuffer *framebuffer = &context.framebuffer;
  struct memory_arena *framebufferArena = &context.framebufferArena;
  {
    framebuffer->width = 1920;
    framebuffer->height = 1080;
    framebuffer->stride = framebuffer->width * sizeof(u32);
    u64 size = framebuffer->height * framebuffer->stride;

    // wl_buffer needs to be aligned to pagesize. see: ERROR_MMAP_WL_SHM
    u64 pagesize = (u64)sysconf(_SC_PAGESIZE);
    framebuffer->data = MemoryArenaPush(framebufferArena, size, pagesize);
  }

  // threads
  /*
  {
    pthread_t outputThread;
    if (pthread_create(&outputThread, 0, OutputThreadMain, &context)) {
      errorTag = ERROR_OUTPUT_PTHREAD_CREATE;
      goto exit;
    }
    pthread_setname_np(outputThread, "vo");
  }
  */

  // wayland
  // - get wayland display
  context.wl_display = wl_display_connect(0);
  if (!context.wl_display) {
    errorTag = ERROR_WL_DISPLAY_CONNECT;
    goto exit;
  }

  // - get wayland registry
  context.wl_registry = wl_display_get_registry(context.wl_display);
  if (!context.wl_registry) {
    errorTag = ERROR_WL_DISPLAY_GET_REGISTRY;
    goto wl_exit;
  }

  // - bind to wayland extensions
  wl_registry_add_listener(context.wl_registry, &wl_registry_listener,
                           &context);
  wl_display_roundtrip(context.wl_display);
  if (!context.wl_compositor || !context.xdg_wm_base || !context.wl_shm ||
      !context.wl_seat || !context.wp_presentation) {
    errorTag = ERROR_WL_REGISTRY_GLOBAL;
    goto wl_exit;
  }

  // - create surface
  context.wl_surface = wl_compositor_create_surface(context.wl_compositor);
  if (!context.wl_surface) {
    errorTag = ERROR_WL_COMPOSITOR_CREATE_SURFACE;
    goto wl_exit;
  }

  // - create window
  context.xdg_surface =
      xdg_wm_base_get_xdg_surface(context.xdg_wm_base, context.wl_surface);
  if (!context.xdg_surface) {
    errorTag = ERROR_XDG_WM_BASE_GET_XDG_SURFACE;
    goto wl_exit;
  }

  xdg_wm_base_add_listener(context.xdg_wm_base, &xdg_wm_base_listener,
                           &context);
  xdg_surface_add_listener(context.xdg_surface, &xdg_surface_listener,
                           &context);

  context.xdg_toplevel = xdg_surface_get_toplevel(context.xdg_surface);
  if (!context.xdg_toplevel) {
    errorTag = ERROR_XDG_SURFACE_GET_TOPLEVEL;
    goto wl_exit;
  }

  xdg_toplevel_set_title(context.xdg_toplevel, "$PROJECT_NAME");
  if (context.wp_content_type_manager_v1) {
    struct wp_content_type_v1 *wp_content_type_v1 =
        wp_content_type_manager_v1_get_surface_content_type(
            context.wp_content_type_manager_v1, context.wl_surface);

    wp_content_type_v1_set_content_type(wp_content_type_v1,
                                        WP_CONTENT_TYPE_V1_TYPE_GAME);
  }

  // - attach framebuffer to window
  {
    s32 fd = memfd_create("wl_shm", 0);
    if (fd == -1) {
      errorTag = ERROR_MEMFD_CREATE_WL_SHM;
      goto wl_exit;
    }

    u64 size = context.framebufferArena.used;
    if (ftruncate(fd, (off_t)size) == -1) {
      close(fd);
      errorTag = ERROR_FTRUNCATE_WL_SHM;
      goto wl_exit;
    }

    u8 *data = mmap(framebuffer->data, size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED, fd, 0);
    if (data == MAP_FAILED) {
      close(fd);
      errorTag = ERROR_MMAP_WL_SHM;
      goto wl_exit;
    }

    struct wl_shm_pool *wl_shm_pool =
        wl_shm_create_pool(context.wl_shm, fd, (s32)size);
    if (!wl_shm_pool) {
      close(fd);
      errorTag = ERROR_WL_SHM_CREATE_POOL;
      goto wl_exit;
    }

    context.wl_buffer = wl_shm_pool_create_buffer(
        wl_shm_pool, 0, framebuffer->width, framebuffer->height,
        framebuffer->stride, WL_SHM_FORMAT_XRGB8888);
    close(fd);
    wl_shm_pool_destroy(wl_shm_pool);
    if (!context.wl_buffer) {
      errorTag = ERROR_WL_SHM_CREATE_POOL;
      goto wl_exit;
    }
  }

  // - draw initial frame
  // must be after creating wl_buffer
  // DrawSolid(framebuffer, 0x3b82f6);
  DrawCheckerBoard(framebuffer, 0xcbd5e1, 0x0f172a, context.offset);

  // - initialize xkb
  context.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!context.xkb_context) {
    errorTag = ERROR_XKB_CONTEXT_NEW;
    goto wl_exit;
  }

  // - get inputs
  wl_seat_add_listener(context.wl_seat, &wl_seat_listener, &context);

  // - commit changes
  wl_surface_commit(context.wl_surface);

  // - register frame callback
  {
    struct wl_callback *wl_surface_frame_callback =
        wl_surface_frame(context.wl_surface);
    wl_callback_add_listener(wl_surface_frame_callback,
                             &wl_surface_frame_listener, &context);
  }

  // event loop
  struct op_poll {
    s32 fd;
  };

  struct op_timer {
    struct __kernel_timespec ts;
  };

  struct io_uring ring;
  if (io_uring_queue_init(4, &ring, 0) != 0) {
    errorTag = ERROR_IO_URING_QUEUE_INIT;
    goto wl_exit;
  }

  // - poll on wl_display
  struct op_poll waylandOp = {};
  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    waylandOp.fd = wl_display_get_fd(context.wl_display);
    io_uring_prep_poll_multishot(sqe, waylandOp.fd, POLLIN);
    io_uring_sqe_set_data(sqe, &waylandOp);
  }

  // - timer
  struct op_timer timerOp = {};
  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    timerOp.ts.tv_nsec = 33333333; // 33.333333ms, 1ms = 1e6 ns

    // one time request
    // io_uring_prep_timeout(sqe, &timerOp.ts, 1, 0);

    // infinite timers at every ts
    io_uring_prep_timeout(sqe, &timerOp.ts, 0, IORING_TIMEOUT_MULTISHOT);
    io_uring_sqe_set_data(sqe, &timerOp);
  }

  io_uring_submit(&ring);

  // - wait for events
  struct io_uring_cqe *cqe;

  u64 previousFrame = Now();
  while (1) {
    while (wl_display_prepare_read(context.wl_display) != 0)
      wl_display_dispatch_pending(context.wl_display);
    wl_display_flush(context.wl_display);

  wait_cqe:
    if (io_uring_wait_cqe(&ring, &cqe) != 0) {
      if (errno == EAGAIN)
        goto wait_cqe;

      errorTag = ERROR_IO_URING_WAIT_CQE;
      break;
    }

    void *data = io_uring_cqe_get_data(cqe);
    if (data != &waylandOp)
      wl_display_cancel_read(context.wl_display);

    // - on wayland events
    if (data == &waylandOp) {
      int revents = cqe->res;
      if (revents & POLLIN)
        wl_display_read_events(context.wl_display);
      else
        wl_display_cancel_read(context.wl_display);
    }

    // - on timer events
    else if (data == &timerOp) {
      u64 now = Now();
      u64 elapsed = now - previousFrame;
      printf("%lu => %luns elapsed\n", now, elapsed);

      struct wayland_present *present = &context.present;
      if (present->isSynced) {
        f32 deltaTime = (f32)elapsed / 1e9f;
        f32 speed = 5.0f;

        context.offset += deltaTime * speed;
        // context.offset += 0.33f;
        DrawCheckerBoard(framebuffer, 0xcbd5e1, 0x0f172a, context.offset);
        wl_surface_attach(context.wl_surface, context.wl_buffer, 0, 0);
        wl_surface_damage_buffer(context.wl_surface, 0, 0, INT32_MAX,
                                 INT32_MAX);

        wl_surface_commit(context.wl_surface);
      } else {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        struct __kernel_timespec ts = {
            .tv_nsec = (long long)(present->ust + present->refresh),
        };
        io_uring_prep_timeout_update(sqe, &ts, (u64)&timerOp,
                                     IORING_TIMEOUT_ABS);
        io_uring_submit(&ring);
        present->isSynced = 1;
      }

      previousFrame = now;
    }

    io_uring_cqe_seen(&ring, cqe);
  }

wl_exit:
  wl_display_disconnect(context.wl_display);

exit:
  return (int)errorTag;
}
