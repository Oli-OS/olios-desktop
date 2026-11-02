#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

struct background {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_output *output;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct wl_surface *cursor_surface;
    struct wl_buffer *cursor_buffer;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_callback *frame_callback;
    struct background_buffer *buffers;
    struct background_buffer *current_buffer;
    uint32_t width;
    uint32_t height;
    bool configured;
    bool output_geometry_received;
    bool cursor_surface_initialized;
    bool fullscreen;
    bool frame_presented;
    bool running;
    bool smoke;
};

struct background_buffer {
    struct background *app;
    struct wl_buffer *buffer;
    cairo_surface_t *surface;
    void *memory;
    size_t memory_size;
    uint32_t width;
    uint32_t height;
    bool current;
    bool released;
    struct background_buffer *next;
};

static void release_buffer(struct background_buffer *frame)
{
    struct background_buffer **cursor = &frame->app->buffers;
    while (*cursor != NULL && *cursor != frame) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == frame) {
        *cursor = frame->next;
    }
    if (frame->buffer != NULL) {
        wl_buffer_destroy(frame->buffer);
    }
    if (frame->surface != NULL) {
        cairo_surface_destroy(frame->surface);
    }
    if (frame->memory != NULL) {
        munmap(frame->memory, frame->memory_size);
    }
    free(frame);
}

static void buffer_released(void *data, struct wl_buffer *buffer)
{
    struct background_buffer *frame = data;
    (void)buffer;
    frame->released = true;
    if (!frame->current) {
        release_buffer(frame);
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_released,
};

static bool create_cursor_surface(struct background *app);

static const char *output_transform_name(int32_t transform)
{
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_NORMAL:
        return "normal";
    case WL_OUTPUT_TRANSFORM_90:
        return "rotate-90";
    case WL_OUTPUT_TRANSFORM_180:
        return "rotate-180";
    case WL_OUTPUT_TRANSFORM_270:
        return "rotate-270";
    case WL_OUTPUT_TRANSFORM_FLIPPED:
        return "flipped";
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        return "flipped-90";
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        return "flipped-180";
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        return "flipped-270";
    default:
        return "unknown";
    }
}

static void output_geometry(void *data, struct wl_output *output, int32_t x,
                            int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform)
{
    struct background *app = data;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    if (output == app->output) {
        app->output_geometry_received = true;
        printf("LUCIA_OUTPUT_TRANSFORM: transform=%s\n",
               output_transform_name(transform));
    }
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)data;
    (void)output;
    (void)flags;
    (void)width;
    (void)height;
    (void)refresh;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct background *app = data;
    (void)surface;
    (void)surface_x;
    (void)surface_y;
    if (app->cursor_surface == NULL && !create_cursor_surface(app)) {
        app->running = false;
        return;
    }
    wl_pointer_set_cursor(pointer, serial, app->cursor_surface, 2, 2);
    if (!app->cursor_surface_initialized) {
        wl_surface_attach(app->cursor_surface, app->cursor_buffer, 0, 0);
        wl_surface_damage(app->cursor_surface, 0, 0, 32, 44);
        app->cursor_surface_initialized = true;
    }
    wl_surface_commit(app->cursor_surface);
    if (wl_display_flush(app->display) < 0) {
        fprintf(stderr, "BACKGROUND_ERROR: send Wayland cursor request\n");
        app->running = false;
        return;
    }
    printf("LUCIA_POINTER_CURSOR_SET: width=32 height=44 hotspot=2,2\n");
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    (void)data;
    (void)pointer;
    (void)serial;
    (void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t surface_x,
                           wl_fixed_t surface_y)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)surface_x;
    (void)surface_y;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    (void)data;
    (void)pointer;
    (void)serial;
    (void)time;
    (void)button;
    (void)state;
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities)
{
    struct background *app = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 &&
        app->pointer == NULL) {
        app->pointer = wl_seat_get_pointer(seat);
        if (app->pointer == NULL ||
            wl_pointer_add_listener(app->pointer, &pointer_listener, app) < 0) {
            fprintf(stderr, "BACKGROUND_ERROR: create Wayland pointer\n");
            if (app->pointer != NULL) {
                wl_pointer_destroy(app->pointer);
                app->pointer = NULL;
            }
            app->running = false;
        }
    } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 &&
               app->pointer != NULL) {
        wl_pointer_destroy(app->pointer);
        app->pointer = NULL;
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
};

static const struct wl_callback_listener frame_listener;

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct background *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(
            registry, name, &xdg_wm_base_interface, version < 1 ? version : 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 &&
               app->seat == NULL) {
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        if (app->seat == NULL ||
            wl_seat_add_listener(app->seat, &seat_listener, app) < 0) {
            fprintf(stderr, "BACKGROUND_ERROR: bind Wayland seat\n");
            if (app->seat != NULL) {
                wl_seat_destroy(app->seat);
                app->seat = NULL;
            }
        }
    } else if (strcmp(interface, wl_output_interface.name) == 0 &&
               app->output == NULL) {
        app->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        if (app->output == NULL ||
            wl_output_add_listener(app->output, &output_listener, app) < 0) {
            fprintf(stderr, "BACKGROUND_ERROR: bind Wayland output\n");
            if (app->output != NULL) {
                wl_output_destroy(app->output);
                app->output = NULL;
            }
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void cursor_buffer_released(void *data, struct wl_buffer *buffer)
{
    (void)data;
    (void)buffer;
}

static const struct wl_buffer_listener cursor_buffer_listener = {
    .release = cursor_buffer_released,
};

static bool create_cursor_surface(struct background *app)
{
    const uint32_t width = 32;
    const uint32_t height = 44;
    const size_t memory_size = (size_t)width * height * sizeof(uint32_t);
    struct wl_shm_pool *pool = NULL;
    struct wl_buffer *buffer = NULL;
    struct wl_surface *surface = NULL;
    cairo_surface_t *image = NULL;
    cairo_t *context = NULL;
    void *memory = MAP_FAILED;
    int fd = memfd_create("lucia-pointer-cursor", MFD_CLOEXEC);
    bool success = false;

    if (fd < 0 || ftruncate(fd, (off_t)memory_size) < 0) {
        perror("BACKGROUND_ERROR: create cursor shared memory");
        goto cleanup;
    }
    memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) {
        perror("BACKGROUND_ERROR: map cursor shared memory");
        goto cleanup;
    }
    image = cairo_image_surface_create_for_data(
        memory, CAIRO_FORMAT_ARGB32, (int)width, (int)height,
        (int)(width * sizeof(uint32_t)));
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "BACKGROUND_ERROR: create cursor image: %s\n",
                cairo_status_to_string(cairo_surface_status(image)));
        goto cleanup;
    }
    context = cairo_create(image);
    cairo_set_operator(context, CAIRO_OPERATOR_CLEAR);
    cairo_paint(context);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);
    cairo_move_to(context, 1, 1);
    cairo_line_to(context, 1, 34);
    cairo_line_to(context, 9, 26);
    cairo_line_to(context, 14, 39);
    cairo_line_to(context, 21, 36);
    cairo_line_to(context, 16, 22);
    cairo_line_to(context, 29, 22);
    cairo_close_path(context);
    cairo_set_source_rgb(context, 0.025, 0.035, 0.030);
    cairo_fill(context);
    cairo_move_to(context, 5, 6);
    cairo_line_to(context, 5, 27);
    cairo_line_to(context, 10, 22);
    cairo_line_to(context, 15, 35);
    cairo_line_to(context, 18, 34);
    cairo_line_to(context, 13, 20);
    cairo_line_to(context, 25, 20);
    cairo_close_path(context);
    cairo_set_source_rgb(context, 0.98, 0.98, 0.93);
    cairo_fill(context);
    cairo_destroy(context);
    context = NULL;
    cairo_surface_flush(image);
    cairo_surface_destroy(image);
    image = NULL;

    pool = wl_shm_create_pool(app->shm, fd, (int)memory_size);
    if (pool == NULL) {
        fprintf(stderr, "BACKGROUND_ERROR: create cursor shared-memory pool\n");
        goto cleanup;
    }
    buffer = wl_shm_pool_create_buffer(
        pool, 0, (int)width, (int)height,
        (int)(width * sizeof(uint32_t)), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    pool = NULL;
    if (buffer == NULL ||
        wl_buffer_add_listener(buffer, &cursor_buffer_listener, app) < 0) {
        fprintf(stderr, "BACKGROUND_ERROR: create cursor Wayland buffer\n");
        goto cleanup;
    }
    surface = wl_compositor_create_surface(app->compositor);
    if (surface == NULL) {
        fprintf(stderr, "BACKGROUND_ERROR: create cursor Wayland surface\n");
        goto cleanup;
    }
    app->cursor_buffer = buffer;
    app->cursor_surface = surface;
    buffer = NULL;
    surface = NULL;
    success = true;

cleanup:
    if (context != NULL) {
        cairo_destroy(context);
    }
    if (image != NULL) {
        cairo_surface_destroy(image);
    }
    if (pool != NULL) {
        wl_shm_pool_destroy(pool);
    }
    if (surface != NULL) {
        wl_surface_destroy(surface);
    }
    if (buffer != NULL) {
        wl_buffer_destroy(buffer);
    }
    if (memory != MAP_FAILED) {
        munmap(memory, memory_size);
    }
    if (fd >= 0) {
        close(fd);
    }
    return success;
}

static void wm_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_ping,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    struct background *app = data;
    uint32_t *state;
    (void)toplevel;
    app->width = width > 0 ? (uint32_t)width : 0;
    app->height = height > 0 ? (uint32_t)height : 0;
    app->fullscreen = false;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN) {
            app->fullscreen = true;
        }
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct background *app = data;
    (void)toplevel;
    app->running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static double clamp(double value, double minimum, double maximum)
{
    if (value < minimum) {
        return minimum;
    }
    return value > maximum ? maximum : value;
}

static struct background_buffer *create_background_buffer(
    struct background *app)
{
    uint64_t pixel_count = (uint64_t)app->width * app->height;
    uint64_t byte_count;
    struct background_buffer *frame = calloc(1, sizeof(*frame));
    struct wl_shm_pool *pool;
    int fd;

    if (app->width == 0 || app->height == 0 ||
        pixel_count > (uint64_t)INT_MAX / sizeof(uint32_t)) {
        fprintf(stderr, "BACKGROUND_ERROR: invalid fullscreen dimensions\n");
        free(frame);
        return NULL;
    }
    byte_count = pixel_count * sizeof(uint32_t);
    if (frame == NULL) {
        perror("BACKGROUND_ERROR: allocate buffer metadata");
        return NULL;
    }
    frame->app = app;
    frame->width = app->width;
    frame->height = app->height;
    frame->memory_size = (size_t)byte_count;
    fd = memfd_create("lucia-background", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)frame->memory_size) < 0) {
        perror("BACKGROUND_ERROR: create shared-memory buffer");
        if (fd >= 0) {
            close(fd);
        }
        free(frame);
        return NULL;
    }
    frame->memory = mmap(NULL, frame->memory_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (frame->memory == MAP_FAILED) {
        frame->memory = NULL;
        perror("BACKGROUND_ERROR: map shared-memory buffer");
        close(fd);
        release_buffer(frame);
        return NULL;
    }
    frame->surface = cairo_image_surface_create_for_data(
        frame->memory, CAIRO_FORMAT_ARGB32, (int)app->width,
        (int)app->height, (int)(app->width * sizeof(uint32_t)));
    if (cairo_surface_status(frame->surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "BACKGROUND_ERROR: create Cairo image surface: %s\n",
                cairo_status_to_string(cairo_surface_status(frame->surface)));
        close(fd);
        release_buffer(frame);
        return NULL;
    }
    pool = wl_shm_create_pool(app->shm, fd, (int)frame->memory_size);
    close(fd);
    if (pool == NULL) {
        fprintf(stderr, "BACKGROUND_ERROR: create Wayland shared-memory pool\n");
        release_buffer(frame);
        return NULL;
    }
    frame->buffer = wl_shm_pool_create_buffer(
        pool, 0, (int)app->width, (int)app->height,
        (int)(app->width * sizeof(uint32_t)), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (frame->buffer == NULL) {
        fprintf(stderr, "BACKGROUND_ERROR: create Wayland buffer\n");
        release_buffer(frame);
        return NULL;
    }
    wl_buffer_add_listener(frame->buffer, &buffer_listener, frame);
    frame->next = app->buffers;
    app->buffers = frame;
    return frame;
}

static void draw_text(cairo_t *context, double x, double y, double size,
                      const char *text, bool bold, double red, double green,
                      double blue)
{
    cairo_select_font_face(context, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, size);
    cairo_set_source_rgb(context, red, green, blue);
    cairo_move_to(context, x, y);
    cairo_show_text(context, text);
}

static void render_desktop(struct background *app,
                           struct background_buffer *frame)
{
    cairo_t *context = cairo_create(frame->surface);
    double width = app->width;
    double height = app->height;
    double width_scale = width / 1440.0;
    double height_scale = height / 900.0;
    double scale = clamp(width_scale < height_scale ? width_scale : height_scale,
                         0.75, 1.35);
    double taskbar_height = clamp(64.0 * scale, 48.0, 72.0);
    double horizontal_padding = clamp(32.0 * scale, 20.0, 42.0);
    double workspace_size = 34.0 * scale;
    double taskbar_y;
    cairo_pattern_t *background = cairo_pattern_create_linear(0, 0, width,
                                                               height);

    if (taskbar_height > height) {
        taskbar_height = height;
    }
    taskbar_y = height - taskbar_height;

    cairo_pattern_add_color_stop_rgb(background, 0.0, 0.105, 0.165, 0.153);
    cairo_pattern_add_color_stop_rgb(background, 0.58, 0.132, 0.209, 0.190);
    cairo_pattern_add_color_stop_rgb(background, 1.0, 0.160, 0.235, 0.207);
    cairo_set_source(context, background);
    cairo_paint(context);
    cairo_pattern_destroy(background);

    cairo_set_source_rgba(context, 0.90, 0.91, 0.83, 0.045);
    cairo_set_line_width(context, 1.0);
    cairo_move_to(context, width * 0.70, 0);
    cairo_line_to(context, width * 0.70, taskbar_y - 1.0);
    cairo_stroke(context);

    cairo_set_source_rgb(context, 0.075, 0.116, 0.108);
    cairo_rectangle(context, 0, taskbar_y, width, taskbar_height);
    cairo_fill(context);
    cairo_set_source_rgb(context, 0.31, 0.46, 0.39);
    cairo_rectangle(context, 0, taskbar_y, width, 1.0);
    cairo_fill(context);

    draw_text(context, horizontal_padding, taskbar_y + taskbar_height * 0.63,
              16.0 * scale, "LUCIA", true, 0.93, 0.94, 0.88);
    cairo_set_source_rgba(context, 0.84, 0.88, 0.80, 0.28);
    cairo_rectangle(context, horizontal_padding + 81.0 * scale,
                    taskbar_y + taskbar_height * 0.27, 1.0,
                    taskbar_height * 0.46);
    cairo_fill(context);

    cairo_set_source_rgb(context, 0.14, 0.22, 0.20);
    cairo_rectangle(context, horizontal_padding + 104.0 * scale,
                    taskbar_y + (taskbar_height - workspace_size) / 2.0,
                    workspace_size, workspace_size);
    cairo_fill(context);
    draw_text(context, horizontal_padding + 104.0 * scale + workspace_size * 0.38,
              taskbar_y + taskbar_height * 0.63, 13.0 * scale, "1", true,
              0.91, 0.93, 0.86);

    cairo_destroy(context);
    cairo_surface_flush(frame->surface);
    printf("LUCIA_TASKBAR_LAYOUT: edge=bottom x=0 y=%u width=%u height=%.0f\n",
           (unsigned int)taskbar_y, app->width, taskbar_height);
    printf("LUCIA_SHELL_ELEMENTS: identity=LUCIA workspace=1 apps=none status=none\n");
}

static void activate_buffer(struct background *app,
                            struct background_buffer *frame)
{
    struct background_buffer *previous = app->current_buffer;
    frame->current = true;
    app->current_buffer = frame;
    if (previous != NULL && previous != frame) {
        previous->current = false;
        if (previous->released) {
            release_buffer(previous);
        }
    }
    render_desktop(app, frame);
    wl_surface_attach(app->surface, frame->buffer, 0, 0);
    wl_surface_damage(app->surface, 0, 0, (int32_t)app->width,
                      (int32_t)app->height);
    if (app->frame_callback != NULL) {
        wl_callback_destroy(app->frame_callback);
    }
    app->frame_callback = wl_surface_frame(app->surface);
    wl_callback_add_listener(app->frame_callback, &frame_listener, app);
    wl_surface_commit(app->surface);
}

static void frame_done(void *data, struct wl_callback *callback,
                       uint32_t callback_data)
{
    struct background *app = data;
    (void)callback_data;
    wl_callback_destroy(callback);
    app->frame_callback = NULL;
    app->frame_presented = true;
    printf("LUCIA_BACKGROUND_FRAME_PRESENTED\n");
    printf("LIFECYCLE: running\n");
    if (app->smoke) {
        app->running = false;
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
                                  uint32_t serial)
{
    struct background *app = data;
    xdg_surface_ack_configure(surface, serial);
    if (!app->fullscreen) {
        fprintf(stderr, "BACKGROUND_ERROR: compositor did not configure fullscreen\n");
        app->running = false;
        return;
    }
    if (app->width == 0 || app->height == 0) {
        fprintf(stderr, "BACKGROUND_ERROR: compositor configured empty dimensions\n");
        app->running = false;
        return;
    }
    if (!app->configured || app->current_buffer == NULL ||
        app->width != app->current_buffer->width ||
        app->height != app->current_buffer->height) {
        struct background_buffer *frame = create_background_buffer(app);
        if (frame == NULL) {
            app->running = false;
            return;
        }
        app->configured = true;
        printf("LUCIA_BACKGROUND_FULLSCREEN_CONFIGURED: %ux%u\n",
               app->width, app->height);
        activate_buffer(app, frame);
    } else {
        wl_surface_commit(app->surface);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void destroy_background(struct background *app)
{
    if (app->frame_callback != NULL) {
        wl_callback_destroy(app->frame_callback);
    }
    while (app->buffers != NULL) {
        release_buffer(app->buffers);
    }
    if (app->pointer != NULL) {
        wl_pointer_destroy(app->pointer);
    }
    if (app->seat != NULL) {
        wl_seat_destroy(app->seat);
    }
    if (app->cursor_surface != NULL) {
        wl_surface_destroy(app->cursor_surface);
    }
    if (app->cursor_buffer != NULL) {
        wl_buffer_destroy(app->cursor_buffer);
    }
    if (app->toplevel != NULL) {
        xdg_toplevel_destroy(app->toplevel);
    }
    if (app->xdg_surface != NULL) {
        xdg_surface_destroy(app->xdg_surface);
    }
    if (app->surface != NULL) {
        wl_surface_destroy(app->surface);
    }
    if (app->wm_base != NULL) {
        xdg_wm_base_destroy(app->wm_base);
    }
    if (app->shm != NULL) {
        wl_shm_destroy(app->shm);
    }
    if (app->compositor != NULL) {
        wl_compositor_destroy(app->compositor);
    }
    if (app->output != NULL) {
        wl_output_destroy(app->output);
    }
    if (app->registry != NULL) {
        wl_registry_destroy(app->registry);
    }
    if (app->display != NULL) {
        wl_display_disconnect(app->display);
    }
}

int main(int argc, char **argv)
{
    struct background app = {.running = true};
    int result = EXIT_FAILURE;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && strcmp(argv[1], "--smoke") == 0) {
        app.smoke = true;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--smoke]\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("LIFECYCLE: startup\n");
    app.display = wl_display_connect(NULL);
    if (app.display == NULL) {
        fprintf(stderr, "BACKGROUND_ERROR: cannot connect to Wayland display\n");
        goto shutdown;
    }
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0 || app.compositor == NULL ||
        app.shm == NULL || app.wm_base == NULL || app.output == NULL) {
        fprintf(stderr, "BACKGROUND_ERROR: required Wayland globals unavailable\n");
        goto shutdown;
    }
    if (!app.output_geometry_received &&
        (wl_display_roundtrip(app.display) < 0 ||
         !app.output_geometry_received)) {
        fprintf(stderr, "BACKGROUND_ERROR: output transform unavailable\n");
        goto shutdown;
    }
    xdg_wm_base_add_listener(app.wm_base, &wm_base_listener, &app);
    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    if (app.surface == NULL || app.xdg_surface == NULL || app.toplevel == NULL) {
        fprintf(stderr, "BACKGROUND_ERROR: create fullscreen surface\n");
        goto shutdown;
    }
    xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_fullscreen(app.toplevel, NULL);
    wl_surface_commit(app.surface);
    while (app.running) {
        if (wl_display_dispatch(app.display) < 0) {
            fprintf(stderr, "BACKGROUND_ERROR: Wayland dispatch failed\n");
            goto shutdown;
        }
    }
    result = app.frame_presented ? EXIT_SUCCESS : EXIT_FAILURE;

shutdown:
    destroy_background(&app);
    printf("LIFECYCLE: shutdown\n");
    return result;
}