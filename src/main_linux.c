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
#include "xdg-shell-client-protocol.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))
#define internal static
#define comptime static const
#define globalvar static

#include "StringBuilder.h"
#include "assert.h"
#include "memory.h"
#include "type.h"

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

internal struct input *InputGetKeyboardAndMouse(struct input *inputs,
                                                u32 inputCount) {
  debug_assert(inputCount != 0);
  struct input *keyboardAndMouseInput = inputs + 0;
  return keyboardAndMouseInput;
}

// TIME
internal u64 Now(void) {
  struct timespec ts;
  debug_assert(clock_gettime(CLOCK_MONOTONIC, &ts) != -1);
  return ((u64)ts.tv_sec * 1000000000 /* 1e9 */) + (u64)ts.tv_nsec;
}

struct linux_context {
  // memory
  struct memory_arena memoryArena;
  struct memory_arena framebufferArena;
  struct memory_arena xkbArena;

  // image
  struct framebuffer framebuffer;

  // string
  struct string_builder stringBuilder;

  // wayland globals
  struct wl_display *wl_display;
  struct wl_registry *wl_registry;
  struct wl_compositor *wl_compositor;
  struct wl_shm *wl_shm;
  struct xdg_wm_base *xdg_wm_base;
  struct wl_seat *wl_seat;
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

  struct io_uring *ring;
  void *gameLoopOp;

  b8 isXDGSurfaceConfigured : 1;
  b8 isWindowClosed : 1;

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
  debug_assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

  struct linux_context *context = data;

  // TODO: manage memory of xkb_keymap allocations
  struct memory_temp xkbKeymapMemory = MemoryTempBegin(&context->xkbArena);
  char *keymapString = (char *)MemoryArenaPush(xkbKeymapMemory.arena, size, 4);
  {
    void *mmapResult =
        mmap(keymapString, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    b8 isKeymapReadFromFile = mmapResult != MAP_FAILED;
    runtime_assert(isKeymapReadFromFile);

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

  struct string_builder *stringBuilder = &context->stringBuilder;
  StringBuilderAppendString(stringBuilder,
                            &STRING_FROM_ZERO_TERMINATED("state "));
  StringBuilderAppendU64(stringBuilder, isPressed);
  StringBuilderAppendString(stringBuilder,
                            &STRING_FROM_ZERO_TERMINATED(" up: "));
  StringBuilderAppendU64(stringBuilder, keyboardAndMouseInput->up.isPressed);
  StringBuilderAppendString(stringBuilder,
                            &STRING_FROM_ZERO_TERMINATED(" down: "));
  StringBuilderAppendU64(stringBuilder, keyboardAndMouseInput->down.isPressed);
  StringBuilderAppendString(stringBuilder,
                            &STRING_FROM_ZERO_TERMINATED(" left: "));
  StringBuilderAppendU64(stringBuilder, keyboardAndMouseInput->left.isPressed);
  StringBuilderAppendString(stringBuilder,
                            &STRING_FROM_ZERO_TERMINATED(" right: "));
  StringBuilderAppendU64(stringBuilder, keyboardAndMouseInput->right.isPressed);
  StringBuilderAppendString(stringBuilder, &STRING_FROM_ZERO_TERMINATED("\n"));
  struct string string = StringBuilderFlush(stringBuilder);
  write(STDOUT_FILENO, string.value, string.length);
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

comptime struct wl_callback_listener wl_surface_frame_listener;
internal void
wl_surface_frame_done(void *data, struct wl_callback *wl_surface_frame_callback,
                      uint32_t nowInMilliseconds) {
  struct linux_context *context = data;

  // - destroy this callback
  wl_callback_destroy(wl_surface_frame_callback);

  // - request another
  wl_surface_frame_callback = wl_surface_frame(context->wl_surface);
  wl_callback_add_listener(wl_surface_frame_callback,
                           &wl_surface_frame_listener, context);
  wl_surface_commit(context->wl_surface);

  // // - presentation feedback
  // struct wp_presentation_feedback *feedback =
  //     wp_presentation_feedback(context->wp_presentation,
  //     context->wl_surface);
  // wp_presentation_feedback_add_listener(
  //     feedback, &wp_presentation_feedback_listener, context);

  {
    globalvar u64 previous = 0;
    u64 now = Now();
    u64 elapsed = now - previous;

    struct string_builder *stringBuilder = &context->stringBuilder;
    StringBuilderAppendString(stringBuilder,
                              &STRING_FROM_ZERO_TERMINATED("frame done fired @: "));
    StringBuilderAppendU64(stringBuilder, now);
    StringBuilderAppendString(stringBuilder,
                              &STRING_FROM_ZERO_TERMINATED(" elapsed: "));
    StringBuilderAppendU64(stringBuilder, elapsed);
    StringBuilderAppendString(stringBuilder,
                              &STRING_FROM_ZERO_TERMINATED("\n"));
    struct string string = StringBuilderFlush(stringBuilder);
    write(STDOUT_FILENO, string.value, string.length);

    previous = now;
  }

  // notify game loop about frame done event
  struct io_uring_sqe *sqe = io_uring_get_sqe(context->ring);
  io_uring_prep_cancel(sqe, context->gameLoopOp, 0);
  io_uring_sqe_set_data(sqe, 0);
  io_uring_submit(context->ring);
}

comptime struct wl_callback_listener wl_surface_frame_listener = {
    .done = wl_surface_frame_done,
};

internal void xdg_toplevel_configure(void *data,
                                     struct xdg_toplevel *xdg_toplevel,
                                     int32_t width, int32_t height,
                                     struct wl_array *states) {}

internal void xdg_toplevel_close(void *data,
                                 struct xdg_toplevel *xdg_toplevel) {
  struct linux_context *context = data;
  context->isWindowClosed = 1;
}

internal void xdg_toplevel_configure_bounds(void *data,
                                            struct xdg_toplevel *xdg_toplevel,
                                            int32_t width, int32_t height) {}

internal void xdg_toplevel_wm_capabilities(void *data,
                                           struct xdg_toplevel *xdg_toplevel,
                                           struct wl_array *capabilities) {}

comptime struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities = xdg_toplevel_wm_capabilities,
};

internal void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                    uint32_t serial) {
  xdg_surface_ack_configure(xdg_surface, serial);

  struct linux_context *context = data;

  if (context->isXDGSurfaceConfigured) {
    // If this isn't the first configure event we've recieved, we already
    // have a buffer attached, so no need to do anything, Commit the
    // surface to apply the configure acknowledgment.
    wl_surface_commit(context->wl_surface);
  }

  context->isXDGSurfaceConfigured = 1;
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

  struct string interfaceString =
      StringFromZeroTerminated((u8 *)interface, 1024);
  if (IsStringEqual(&interfaceString,
                    &STRING_FROM_ZERO_TERMINATED("wl_compositor"))) {
    context->wl_compositor =
        wl_registry_bind(wl_registry, name, &wl_compositor_interface, version);
  } else if (IsStringEqual(&interfaceString,
                           &STRING_FROM_ZERO_TERMINATED("wl_shm"))) {
    context->wl_shm =
        wl_registry_bind(wl_registry, name, &wl_shm_interface, version);
  } else if (IsStringEqual(&interfaceString,
                           &STRING_FROM_ZERO_TERMINATED("xdg_wm_base"))) {
    context->xdg_wm_base =
        wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, version);
  } else if (IsStringEqual(&interfaceString,
                           &STRING_FROM_ZERO_TERMINATED("wl_seat"))) {
    context->wl_seat =
        wl_registry_bind(wl_registry, name, &wl_seat_interface, version);
  } else if (IsStringEqual(
                 &interfaceString,
                 &STRING_FROM_ZERO_TERMINATED("wp_content_type_manager_v1"))) {
    context->wp_content_type_manager_v1 = wl_registry_bind(
        wl_registry, name, &wp_content_type_manager_v1_interface, version);
  }
}

comptime struct wl_registry_listener wl_registry_listener = {
    .global = wl_registry_global,
};

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
        .total = totalMemoryAllocated,
        .block = mmap(0, totalMemoryAllocated, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
    };
    if (memoryArena->block == MAP_FAILED) {
      errorTag = ERROR_MMAP;
      goto exit;
    }

    // TODO: tweak this
    // 1920x1080x4 = ~7.91m
    context.framebufferArena = MemoryArenaSub(memoryArena, 8 * MEGABYTES);

    // TODO: tweak this
    context.xkbArena = MemoryArenaSub(memoryArena, 1 * MEGABYTES);
  }

  // string builder
  struct string_builder *stringBuilder = &context.stringBuilder;
  struct string stdoutBuffer = MemoryArenaPushString(memoryArena, 512);
  struct string stringBuffer = MemoryArenaPushString(memoryArena, 32);
  stringBuilder->outBuffer = &stdoutBuffer;
  stringBuilder->stringBuffer = &stringBuffer;

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

  // - initialize xkb
  context.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!context.xkb_context) {
    errorTag = ERROR_XKB_CONTEXT_NEW;
    goto exit;
  }

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
      !context.wl_seat) {
    errorTag = ERROR_WL_REGISTRY_GLOBAL;
    goto wl_exit;
  }

  // - create surface
  context.wl_surface = wl_compositor_create_surface(context.wl_compositor);
  if (!context.wl_surface) {
    errorTag = ERROR_WL_COMPOSITOR_CREATE_SURFACE;
    goto wl_exit;
  }

  // - get inputs
  wl_seat_add_listener(context.wl_seat, &wl_seat_listener, &context);

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

  xdg_toplevel_add_listener(context.xdg_toplevel, &xdg_toplevel_listener,
                            &context);

  xdg_toplevel_set_title(context.xdg_toplevel, "$PROJECT_NAME");
  if (context.wp_content_type_manager_v1) {
    struct wp_content_type_v1 *wp_content_type_v1 =
        wp_content_type_manager_v1_get_surface_content_type(
            context.wp_content_type_manager_v1, context.wl_surface);

    wp_content_type_v1_set_content_type(wp_content_type_v1,
                                        WP_CONTENT_TYPE_V1_TYPE_GAME);
  }

  // - Perform the initial commit and wait for first configure event
  wl_surface_commit(context.wl_surface);
  while (wl_display_dispatch(context.wl_display) != -1 &&
         !context.isXDGSurfaceConfigured) {
    // intentionally left blank
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

    // - draw initial frame
    // must be after creating wl_buffer
    // DrawSolid(framebuffer, 0x3b82f6);
    DrawCheckerBoard(framebuffer, 0xcbd5e1, 0x0f172a, context.offset);
    wl_surface_attach(context.wl_surface, context.wl_buffer, 0, 0);
  }

  // - register frame callback
  {
    struct wl_callback *wl_surface_frame_callback =
        wl_surface_frame(context.wl_surface);
    wl_callback_add_listener(wl_surface_frame_callback,
                             &wl_surface_frame_listener, &context);
  }

  // - commit changes
  wl_surface_commit(context.wl_surface);

  // event loop
  struct op {};

  struct op_timer {
    struct __kernel_timespec ts;
  };

  struct io_uring ring;
  {
    struct io_uring_params params = {
        .features = IORING_FEAT_SUBMIT_STABLE,
    };
    if (io_uring_queue_init_params(4, &ring, &params) != 0) {
      errorTag = ERROR_IO_URING_QUEUE_INIT;
      goto wl_exit;
    }

    context.ring = &ring;
  }

  // - poll on wl_display
  struct op waylandOp = {};
  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    int fd = wl_display_get_fd(context.wl_display);
    io_uring_prep_poll_multishot(sqe, fd, POLLIN);
    io_uring_sqe_set_data(sqe, &waylandOp);
  }

  // - game loop op
  struct op_timer gameLoopOp = {};
  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    gameLoopOp.ts.tv_nsec = 33333333; // 33.333333ms, 1ms = 1e6 ns

    // infinite timers at every ts
    io_uring_prep_timeout(sqe, &gameLoopOp.ts, 0, IORING_TIMEOUT_MULTISHOT);
    io_uring_sqe_set_data(sqe, &gameLoopOp);

    context.gameLoopOp = &gameLoopOp;
  }

  io_uring_submit(&ring);

  // - wait for events
  struct io_uring_cqe *cqe;

  u64 previousFrame = Now();
  while (!context.isWindowClosed) {
    while (wl_display_prepare_read(context.wl_display) != 0)
      wl_display_dispatch_pending(context.wl_display);
    wl_display_flush(context.wl_display);

    int io_uring_wait_err;
  wait_cqe:
    io_uring_wait_err = io_uring_wait_cqe(&ring, &cqe);
    if (io_uring_wait_err != 0) {
      io_uring_wait_err *= -1;
      if (io_uring_wait_err == EAGAIN || io_uring_wait_err == EINTR)
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

    // - on game loop events
    else if (data == &gameLoopOp) {
      /* Wayland when app is not in focus, stops sending frame done events.
       * Games do not stop sending update events when app becomes invisible.
       * (e.g. music plaing in background, physics simulation goes berserk when
       * delta time is huge) I solve this by sleeping with intervals of 33.33ms
       * when app is in background and using frame done callback when it is in
       * foreground.
       */
      b8 isFrameDoneEvent = cqe->res == -ECANCELED;

      u64 now = Now();
      u64 elapsed = now - previousFrame;

      u64 targetPerFrameInNanoseconds = 33000000 /* 33.333333ms */;
      if (elapsed >= targetPerFrameInNanoseconds) {
        f32 deltaTime = (f32)elapsed / 1e9f;
        f32 speed = 5.0f;
        context.offset += deltaTime * speed;

        // print message
        {
          StringBuilderAppendString(
              stringBuilder, &STRING_FROM_ZERO_TERMINATED("frame done: "));
          StringBuilderAppendU64(stringBuilder, isFrameDoneEvent);
          StringBuilderAppendString(stringBuilder,
                                    &STRING_FROM_ZERO_TERMINATED(" time: "));
          StringBuilderAppendU64(stringBuilder, now);
          StringBuilderAppendString(stringBuilder,
                                    &STRING_FROM_ZERO_TERMINATED(" elapsed: "));
          StringBuilderAppendU64(stringBuilder, elapsed);
          StringBuilderAppendString(stringBuilder,
                                    &STRING_FROM_ZERO_TERMINATED(" offset: "));
          StringBuilderAppendF32(stringBuilder, context.offset, 2);
          StringBuilderAppendString(stringBuilder,
                                    &STRING_FROM_ZERO_TERMINATED("\n"));
          struct string string = StringBuilderFlush(stringBuilder);
          write(STDOUT_FILENO, string.value, string.length);
        }

        // update frame
        DrawCheckerBoard(framebuffer, 0xcbd5e1, 0x0f172a, context.offset);

        previousFrame = now;
      }

      if (isFrameDoneEvent) {
        // swap buffers when frame done
        wl_surface_attach(context.wl_surface, context.wl_buffer, 0, 0);
        wl_surface_damage_buffer(context.wl_surface, 0, 0, INT32_MAX,
                                 INT32_MAX);
        wl_surface_commit(context.wl_surface);

        // - rearm timer
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_timeout(sqe, &gameLoopOp.ts, 0, IORING_TIMEOUT_MULTISHOT);
        io_uring_sqe_set_data(sqe, &gameLoopOp);
        io_uring_submit(&ring);
      }
    }

    io_uring_cqe_seen(&ring, cqe);
  }

  io_uring_queue_exit(&ring);

  xdg_toplevel_destroy(context.xdg_toplevel);
  xdg_surface_destroy(context.xdg_surface);
  wl_surface_destroy(context.wl_surface);
  wl_buffer_destroy(context.wl_buffer);

wl_exit:
  wl_display_disconnect(context.wl_display);

exit:
  return (int)errorTag;
}
