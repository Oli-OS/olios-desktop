#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

#define LOGICAL_WIDTH 480
#define LOGICAL_HEIGHT 320
#define BUFFER_SCALE 2
#define BUFFER_WIDTH (LOGICAL_WIDTH * BUFFER_SCALE)
#define BUFFER_HEIGHT (LOGICAL_HEIGHT * BUFFER_SCALE)
#define BUFFER_COUNT 2
#define BUTTON_X 32.0
#define BUTTON_Y 180.0
#define BUTTON_WIDTH 416.0
#define BUTTON_HEIGHT 56.0
#define KEY_TAB 15
#define KEY_ENTER 28
#define KEY_KPENTER 96
#define KEY_SPACE 57
#define KEY_LEFT 105

struct app;

struct frame_buffer {
    struct app *app;
    struct wl_buffer *buffer;
    cairo_surface_t *surface;
    bool busy;
};

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_callback *frame_callback;
    struct frame_buffer buffers[BUFFER_COUNT];
    void *buffer_memory;
    size_t buffer_memory_size;
    bool configured;
    bool frame_done;
    bool presented;
    bool running;
    bool smoke;
    bool keyboard_focus;
    bool control_focus;
    bool pointer_inside;
    bool pointer_pressed;
    double pointer_x;
    double pointer_y;
    const char *status;
};

static const char status_label[] = "Status";
static const char status_role[] = "status";
static const char status_name[] = "Status";
static const char button_label[] = "Activate";
static const char button_role[] = "button";
static const char button_name[] = "Activate";

static void render(struct app *app);

static bool point_in_button(double x, double y)
{
    return x >= BUTTON_X && x <= BUTTON_X + BUTTON_WIDTH && y >= BUTTON_Y &&
           y <= BUTTON_Y + BUTTON_HEIGHT;
}

static void activate(struct app *app, const char *source)
{
    app->status = source;
    if (strcmp(source, "pointer") == 0) {
        app->status = "Activated by pointer";
    } else {
        app->status = "Activated by keyboard";
    }
    printf("BUTTON_ACTIVATED: source=%s\n", source);
    printf("STATUS_UPDATED: %s\n", app->status);
    if (app->smoke) {
        printf("%s_ACTIVATION_OK\n",
               strcmp(source, "pointer") == 0 ? "POINTER" : "KEYBOARD");
    }
    render(app);
}

static void set_pointer_position(struct app *app, double x, double y)
{
    bool was_inside = app->pointer_inside;
    bool inside = point_in_button(x, y);
    app->pointer_x = x;
    app->pointer_y = y;
    app->pointer_inside = inside;
    if (was_inside != inside) {
        render(app);
    }
}

static void pointer_entered(struct app *app, double x, double y)
{
    set_pointer_position(app, x, y);
}

static void pointer_left(struct app *app)
{
    app->pointer_inside = false;
    app->pointer_pressed = false;
    render(app);
}

static void pointer_button(struct app *app, uint32_t button, uint32_t state)
{
    if (button != 0x110) {
        return;
    }
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        app->pointer_pressed = point_in_button(app->pointer_x, app->pointer_y);
        if (app->pointer_pressed) {
            app->control_focus = true;
            render(app);
        }
    } else if (app->pointer_pressed) {
        bool activate_button = point_in_button(app->pointer_x, app->pointer_y);
        app->pointer_pressed = false;
        if (activate_button) {
            activate(app, "pointer");
        } else {
            render(app);
        }
    }
}

static void keyboard_entered(struct app *app)
{
    app->keyboard_focus = true;
    render(app);
}

static void keyboard_left(struct app *app)
{
    app->keyboard_focus = false;
    render(app);
}

static void keyboard_key(struct app *app, uint32_t key, uint32_t state)
{
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        return;
    }
    if (key == KEY_TAB) {
        app->control_focus = true;
        printf("FOCUS_VISIBLE: button\n");
        render(app);
        return;
    }
    if (app->keyboard_focus && app->control_focus &&
        (key == KEY_ENTER || key == KEY_KPENTER || key == KEY_SPACE)) {
        activate(app, "keyboard");
    }
}

static void frame_done(void *data, struct wl_callback *callback,
                       uint32_t callback_data)
{
    struct app *app = data;
    (void)callback_data;
    wl_callback_destroy(callback);
    app->frame_callback = NULL;
    app->frame_done = true;
    if (!app->presented) {
        app->presented = true;
        printf("FRAME_PRESENTED\n");
        printf("LIFECYCLE: running\n");
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void buffer_released(void *data, struct wl_buffer *buffer)
{
    struct frame_buffer *frame = data;
    (void)buffer;
    frame->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_released,
};

static void draw_text(cairo_t *context, double x, double y, double size,
                      const char *text, double red, double green, double blue)
{
    cairo_select_font_face(context, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, size);
    cairo_set_source_rgb(context, red, green, blue);
    cairo_move_to(context, x, y);
    cairo_show_text(context, text);
}

static void render_buffer(struct app *app, struct frame_buffer *frame)
{
    cairo_t *context = cairo_create(frame->surface);
    bool focused = app->keyboard_focus && app->control_focus;
    bool hovered = app->pointer_inside;
    double red = focused ? 0.04 : (hovered ? 0.10 : 0.28);
    double green = focused ? 0.43 : (hovered ? 0.44 : 0.34);
    double blue = focused ? 0.40 : (hovered ? 0.38 : 0.40);

    cairo_scale(context, BUFFER_SCALE, BUFFER_SCALE);
    cairo_set_source_rgb(context, 0.96, 0.96, 0.93);
    cairo_paint(context);

    draw_text(context, 32, 54, 24, "LUCIA GUI Prototype", 0.12, 0.16, 0.17);
    draw_text(context, 32, 112, 14, "Status", 0.28, 0.34, 0.34);
    draw_text(context, 32, 140, 18, app->status, 0.12, 0.16, 0.17);

    cairo_rectangle(context, BUTTON_X, BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT);
    cairo_set_source_rgb(context, 0.99, 0.99, 0.97);
    cairo_fill_preserve(context);
    cairo_set_line_width(context, focused ? 3.0 : 1.5);
    cairo_set_source_rgb(context, red, green, blue);
    cairo_stroke(context);
    draw_text(context, BUTTON_X + 16, BUTTON_Y + 35, 18, button_label, red,
              green, blue);
    if (focused) {
        cairo_set_line_width(context, 1.5);
        cairo_set_source_rgb(context, 0.04, 0.43, 0.40);
        cairo_rectangle(context, BUTTON_X - 4, BUTTON_Y - 4,
                        BUTTON_WIDTH + 8, BUTTON_HEIGHT + 8);
        cairo_stroke(context);
    }

    cairo_destroy(context);
    cairo_surface_flush(frame->surface);
    frame->busy = true;
    wl_surface_attach(app->surface, frame->buffer, 0, 0);
    wl_surface_damage(app->surface, 0, 0, LOGICAL_WIDTH, LOGICAL_HEIGHT);
    if (app->frame_callback != NULL) {
        wl_callback_destroy(app->frame_callback);
    }
    app->frame_done = false;
    app->frame_callback = wl_surface_frame(app->surface);
    wl_callback_add_listener(app->frame_callback, &frame_listener, app);
    wl_surface_commit(app->surface);
}

static void render(struct app *app)
{
    size_t index;
    if (!app->configured || app->surface == NULL) {
        return;
    }
    for (index = 0; index < BUFFER_COUNT; index++) {
        if (!app->buffers[index].busy) {
            render_buffer(app, &app->buffers[index]);
            return;
        }
    }
}

static int create_buffers(struct app *app)
{
    const size_t stride = (size_t)BUFFER_WIDTH * 4;
    const size_t buffer_size = stride * BUFFER_HEIGHT;
    struct wl_shm_pool *pool;
    int fd = memfd_create("lucia-gui-prototype", MFD_CLOEXEC);
    size_t index;

    if (fd < 0 || ftruncate(fd, (off_t)(buffer_size * BUFFER_COUNT)) < 0) {
        perror("STARTUP_ERROR: create shared-memory buffer");
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    app->buffer_memory_size = buffer_size * BUFFER_COUNT;
    app->buffer_memory = mmap(NULL, app->buffer_memory_size,
                              PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (app->buffer_memory == MAP_FAILED) {
        app->buffer_memory = NULL;
        perror("STARTUP_ERROR: map shared-memory buffer");
        close(fd);
        return -1;
    }
    pool = wl_shm_create_pool(app->shm, fd,
                              (int)(buffer_size * BUFFER_COUNT));
    close(fd);
    if (pool == NULL) {
        fprintf(stderr, "STARTUP_ERROR: create Wayland shared-memory pool\n");
        return -1;
    }

    for (index = 0; index < BUFFER_COUNT; index++) {
        struct frame_buffer *frame = &app->buffers[index];
        unsigned char *pixels = (unsigned char *)app->buffer_memory +
                                (buffer_size * index);
        frame->app = app;
        frame->buffer = wl_shm_pool_create_buffer(
            pool, (int)(buffer_size * index), BUFFER_WIDTH, BUFFER_HEIGHT,
            (int)stride, WL_SHM_FORMAT_ARGB8888);
        if (frame->buffer == NULL ||
            wl_buffer_add_listener(frame->buffer, &buffer_listener, frame) < 0) {
            fprintf(stderr, "STARTUP_ERROR: create Wayland buffer\n");
            wl_shm_pool_destroy(pool);
            return -1;
        }
        frame->surface = cairo_image_surface_create_for_data(
            pixels, CAIRO_FORMAT_ARGB32, BUFFER_WIDTH, BUFFER_HEIGHT,
            (int)stride);
        if (cairo_surface_status(frame->surface) != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "STARTUP_ERROR: create Cairo image surface\n");
            wl_shm_pool_destroy(pool);
            return -1;
        }
    }
    wl_shm_pool_destroy(pool);
    return 0;
}

static void wm_ping(void *data, struct xdg_wm_base *wm_base,
                    uint32_t serial)
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
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct app *app = data;
    (void)toplevel;
    app->running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                 uint32_t serial)
{
    struct app *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    app->configured = true;
    printf("WINDOW_CONFIGURED\n");
    render(app);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct app *app = data;
    (void)pointer;
    (void)serial;
    (void)surface;
    pointer_entered(app, wl_fixed_to_double(surface_x),
                    wl_fixed_to_double(surface_y));
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    pointer_left(data);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t surface_x,
                           wl_fixed_t surface_y)
{
    struct app *app = data;
    (void)pointer;
    (void)time;
    set_pointer_position(app, wl_fixed_to_double(surface_x),
                         wl_fixed_to_double(surface_y));
}

static void pointer_button_event(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, uint32_t time,
                                 uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)serial;
    (void)time;
    pointer_button(data, button, state);
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
    .button = pointer_button_event,
    .axis = pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
    keyboard_entered(data);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    keyboard_left(data);
}

static void keyboard_key_event(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t time, uint32_t key,
                               uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    keyboard_key(data, key, state);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)depressed;
    (void)latched;
    (void)locked;
    (void)group;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key_event,
    .modifiers = keyboard_modifiers,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities)
{
    struct app *app = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 &&
        app->pointer == NULL) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 &&
               app->pointer != NULL) {
        wl_pointer_destroy(app->pointer);
        app->pointer = NULL;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 &&
        app->keyboard == NULL) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    } else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0 &&
               app->keyboard != NULL) {
        wl_keyboard_destroy(app->keyboard);
        app->keyboard = NULL;
        keyboard_left(app);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
};

static uint32_t bind_version(uint32_t advertised, uint32_t supported)
{
    return advertised < supported ? advertised : supported;
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct app *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface,
            bind_version(version, 3));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(
            registry, name, &xdg_wm_base_interface,
            bind_version(version, 1));
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
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

static int wait_for_frame(struct app *app)
{
    while (!app->frame_done && app->running) {
        if (wl_display_dispatch(app->display) < 0) {
            fprintf(stderr, "RUNTIME_ERROR: Wayland dispatch failed\n");
            return -1;
        }
    }
    return app->frame_done ? 0 : -1;
}

static int run_smoke(struct app *app)
{
    if (wait_for_frame(app) < 0) {
        return -1;
    }
    app->keyboard_focus = true;
    keyboard_entered(app);
    keyboard_key(app, KEY_TAB, WL_KEYBOARD_KEY_STATE_PRESSED);
    if (wait_for_frame(app) < 0) {
        return -1;
    }
    keyboard_key(app, KEY_ENTER, WL_KEYBOARD_KEY_STATE_PRESSED);
    if (wait_for_frame(app) < 0 ||
        strcmp(app->status, "Activated by keyboard") != 0) {
        fprintf(stderr, "RUNTIME_ERROR: keyboard activation smoke failed\n");
        return -1;
    }

    app->status = "Ready";
    render(app);
    if (wait_for_frame(app) < 0) {
        return -1;
    }
    pointer_entered(app, BUTTON_X + 8, BUTTON_Y + 8);
    if (wait_for_frame(app) < 0) {
        return -1;
    }
    pointer_button(app, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    if (wait_for_frame(app) < 0) {
        return -1;
    }
    pointer_button(app, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    if (wait_for_frame(app) < 0 ||
        strcmp(app->status, "Activated by pointer") != 0) {
        fprintf(stderr, "RUNTIME_ERROR: pointer activation smoke failed\n");
        return -1;
    }
    app->running = false;
    return 0;
}

static void destroy_app(struct app *app)
{
    size_t index;
    if (app->frame_callback != NULL) {
        wl_callback_destroy(app->frame_callback);
    }
    for (index = 0; index < BUFFER_COUNT; index++) {
        if (app->buffers[index].surface != NULL) {
            cairo_surface_destroy(app->buffers[index].surface);
        }
        if (app->buffers[index].buffer != NULL) {
            wl_buffer_destroy(app->buffers[index].buffer);
        }
    }
    if (app->buffer_memory != NULL) {
        munmap(app->buffer_memory, app->buffer_memory_size);
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
    if (app->keyboard != NULL) {
        wl_keyboard_destroy(app->keyboard);
    }
    if (app->pointer != NULL) {
        wl_pointer_destroy(app->pointer);
    }
    if (app->seat != NULL) {
        wl_seat_destroy(app->seat);
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
    if (app->registry != NULL) {
        wl_registry_destroy(app->registry);
    }
    if (app->display != NULL) {
        wl_display_disconnect(app->display);
    }
}

int main(int argc, char **argv)
{
    struct app app = {.running = true, .status = "Ready"};
    int result = EXIT_FAILURE;

    if (argc == 2 && strcmp(argv[1], "--smoke") == 0) {
        app.smoke = true;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--smoke]\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("LIFECYCLE: startup\n");
    app.display = wl_display_connect(NULL);
    if (app.display == NULL) {
        fprintf(stderr, "STARTUP_ERROR: cannot connect to Wayland display\n");
        goto shutdown;
    }
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0 || app.compositor == NULL ||
        app.shm == NULL || app.wm_base == NULL) {
        fprintf(stderr, "STARTUP_ERROR: required Wayland globals unavailable\n");
        goto shutdown;
    }
    if (wl_compositor_get_version(app.compositor) < 3) {
        fprintf(stderr, "STARTUP_ERROR: compositor lacks logical buffer scale\n");
        goto shutdown;
    }

    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_title(app.toplevel, "LUCIA GUI Prototype");
    xdg_toplevel_set_app_id(app.toplevel, "org.lucia.gui-prototype");
    xdg_toplevel_set_min_size(app.toplevel, LOGICAL_WIDTH, LOGICAL_HEIGHT);
    xdg_toplevel_set_max_size(app.toplevel, LOGICAL_WIDTH, LOGICAL_HEIGHT);
    xdg_surface_set_window_geometry(app.xdg_surface, 0, 0, LOGICAL_WIDTH,
                                    LOGICAL_HEIGHT);
    if (create_buffers(&app) < 0) {
        goto shutdown;
    }
    wl_surface_set_buffer_scale(app.surface, BUFFER_SCALE);
    wl_surface_commit(app.surface);
    if (wl_display_roundtrip(app.display) < 0 || !app.configured) {
        fprintf(stderr, "STARTUP_ERROR: window configuration failed\n");
        goto shutdown;
    }
    printf("WAYLAND_CONNECTED\n");
    printf("LOGICAL_SIZE: %dx%d buffer_scale=%d buffer=%dx%d\n",
           LOGICAL_WIDTH, LOGICAL_HEIGHT, BUFFER_SCALE, BUFFER_WIDTH,
           BUFFER_HEIGHT);
    printf("ACCESSIBILITY_METADATA: label=%s role=%s name=%s\n", status_label,
           status_role, status_name);
    printf("ACCESSIBILITY_METADATA: label=%s role=%s name=%s\n", button_label,
           button_role, button_name);
    printf("ACCESSIBILITY_EXPOSURE: client-model-only\n");
    render(&app);

    if (app.smoke) {
        result = run_smoke(&app) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } else {
        while (app.running) {
            if (wl_display_dispatch(app.display) < 0) {
                fprintf(stderr, "RUNTIME_ERROR: Wayland dispatch failed\n");
                result = EXIT_FAILURE;
                goto shutdown;
            }
        }
        result = EXIT_SUCCESS;
    }

shutdown:
    printf("LIFECYCLE: shutdown\n");
    destroy_app(&app);
    return result;
}