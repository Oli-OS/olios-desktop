#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "xdg-shell-client-protocol.h"

struct background {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
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
    bool assistant_open;
    bool keyboard_focus;
    bool has_submission;
    bool pointer_position_valid;
    bool running;
    bool smoke;
    bool request_active;
    bool provider_connecting;
    bool response_error;
    double pointer_x;
    double pointer_y;
    int provider_fd;
    size_t request_length;
    size_t request_sent;
    size_t response_header_received;
    size_t response_length;
    size_t response_received;
    unsigned char request_frame[4 + 512];
    unsigned char response_header[5];
    char prompt[512];
    char submitted_prompt[512];
    char response[16385];
    char status[256];
};

struct desktop_layout {
    double scale;
    double taskbar_y;
    double taskbar_height;
    double menu_x;
    double menu_y;
    double menu_width;
    double menu_height;
    double pin_size;
    double pin_gap;
    double panel_x;
    double panel_y;
    double panel_width;
    double panel_height;
    double prompt_x;
    double prompt_y;
    double prompt_width;
    double prompt_height;
    double submit_x;
    double submit_width;
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
static struct desktop_layout calculate_layout(struct background *app);
static void request_redraw(struct background *app);
static void cancel_request(struct background *app);
static void submit_prompt(struct background *app);
static void clear_conversation(struct background *app);

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
    app->pointer_x = wl_fixed_to_double(surface_x);
    app->pointer_y = wl_fixed_to_double(surface_y);
    app->pointer_position_valid = surface == app->surface;
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
    struct background *app = data;
    (void)pointer;
    (void)serial;
    (void)surface;
    app->pointer_position_valid = false;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t surface_x,
                           wl_fixed_t surface_y)
{
    struct background *app = data;
    (void)pointer;
    (void)time;
    app->pointer_x = wl_fixed_to_double(surface_x);
    app->pointer_y = wl_fixed_to_double(surface_y);
    app->pointer_position_valid = true;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    struct background *app = data;
    (void)pointer;
    (void)serial;
    (void)time;
    struct desktop_layout layout;
    if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED ||
        !app->pointer_position_valid || !app->configured) {
        return;
    }
    layout = calculate_layout(app);
    if (app->assistant_open &&
        app->pointer_x >= layout.panel_x + layout.panel_width - 44.0 &&
        app->pointer_x <= layout.panel_x + layout.panel_width - 12.0 &&
        app->pointer_y >= layout.panel_y + 12.0 &&
        app->pointer_y <= layout.panel_y + 44.0) {
        cancel_request(app);
        app->assistant_open = false;
        request_redraw(app);
    } else if (app->assistant_open &&
               app->pointer_x >= layout.panel_x + layout.panel_width - 108.0 &&
               app->pointer_x <= layout.panel_x + layout.panel_width - 58.0 &&
               app->pointer_y >= layout.panel_y + 12.0 &&
               app->pointer_y <= layout.panel_y + 44.0) {
        clear_conversation(app);
    } else if (app->assistant_open &&
               app->pointer_x >= layout.submit_x &&
               app->pointer_x <= layout.submit_x + layout.submit_width &&
               app->pointer_y >= layout.prompt_y &&
               app->pointer_y <= layout.prompt_y + layout.prompt_height) {
        if (app->request_active) {
            cancel_request(app);
        } else {
            submit_prompt(app);
        }
    } else if (app->pointer_x >= layout.menu_x &&
               app->pointer_x <= layout.menu_x + layout.menu_width &&
               app->pointer_y >= layout.menu_y &&
               app->pointer_y <= layout.menu_y + layout.menu_height) {
        if (app->assistant_open) {
            cancel_request(app);
        }
        app->assistant_open = !app->assistant_open;
        request_redraw(app);
    }
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

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    struct background *app = data;
    void *mapping = MAP_FAILED;
    char *keymap_text = NULL;
    struct xkb_keymap *keymap = NULL;
    struct xkb_state *state = NULL;
    (void)keyboard;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 ||
        app->xkb_context == NULL) {
        close(fd);
        return;
    }
    mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        return;
    }
    keymap_text = malloc((size_t)size + 1);
    if (keymap_text != NULL) {
        memcpy(keymap_text, mapping, size);
        keymap_text[size] = '\0';
        keymap = xkb_keymap_new_from_string(
            app->xkb_context, keymap_text, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap != NULL) {
            state = xkb_state_new(keymap);
        }
    }
    free(keymap_text);
    munmap(mapping, size);
    if (keymap == NULL || state == NULL) {
        if (state != NULL) {
            xkb_state_unref(state);
        }
        if (keymap != NULL) {
            xkb_keymap_unref(keymap);
        }
        fprintf(stderr, "BACKGROUND_WARNING: unavailable keyboard keymap\n");
        return;
    }
    if (app->xkb_state != NULL) {
        xkb_state_unref(app->xkb_state);
    }
    if (app->xkb_keymap != NULL) {
        xkb_keymap_unref(app->xkb_keymap);
    }
    app->xkb_keymap = keymap;
    app->xkb_state = state;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    struct background *app = data;
    (void)keyboard;
    (void)serial;
    (void)keys;
    app->keyboard_focus = surface == app->surface;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    struct background *app = data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    app->keyboard_focus = false;
}

static void submit_prompt(struct background *app)
{
    const char *socket_path = getenv("LUCIA_MODEL_SOCKET");
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t prompt_length = strlen(app->prompt);
    size_t socket_path_length;
    int connection_result;

    if (app->prompt[0] == '\0') {
        return;
    }
    if (app->request_active) {
        return;
    }
    if (socket_path == NULL || socket_path[0] == '\0') {
        snprintf(app->status, sizeof(app->status),
                 "Local model socket is not configured.");
        app->response_error = true;
        request_redraw(app);
        return;
    }
    socket_path_length = strnlen(socket_path, sizeof(address.sun_path));
    if (socket_path_length == 0 || socket_path_length >= sizeof(address.sun_path)) {
        snprintf(app->status, sizeof(app->status),
                 "Local model socket path is invalid.");
        app->response_error = true;
        request_redraw(app);
        return;
    }
    memcpy(address.sun_path, socket_path, socket_path_length + 1);
    app->provider_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (app->provider_fd < 0) {
        snprintf(app->status, sizeof(app->status),
                 "Cannot open local model socket: %.180s", strerror(errno));
        app->response_error = true;
        request_redraw(app);
        return;
    }
    app->request_frame[0] = (unsigned char)((prompt_length >> 24) & 0xff);
    app->request_frame[1] = (unsigned char)((prompt_length >> 16) & 0xff);
    app->request_frame[2] = (unsigned char)((prompt_length >> 8) & 0xff);
    app->request_frame[3] = (unsigned char)(prompt_length & 0xff);
    memcpy(app->request_frame + 4, app->prompt, prompt_length);
    app->request_length = prompt_length + 4;
    app->request_sent = 0;
    app->response_header_received = 0;
    app->response_length = 0;
    app->response_received = 0;
    app->response[0] = '\0';
    app->response_error = false;
    connection_result = connect(app->provider_fd,
                                (struct sockaddr *)&address,
                                (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                            socket_path_length + 1));
    if (connection_result < 0 && errno != EINPROGRESS) {
        snprintf(app->status, sizeof(app->status),
                 "Local model unavailable: %.180s", strerror(errno));
        close(app->provider_fd);
        app->provider_fd = -1;
        app->response_error = true;
        request_redraw(app);
        return;
    }
    app->provider_connecting = connection_result < 0;
    app->request_active = true;
    memcpy(app->submitted_prompt, app->prompt, sizeof(app->submitted_prompt));
    app->prompt[0] = '\0';
    app->has_submission = true;
    snprintf(app->status, sizeof(app->status), "Connecting to qwen3:0.6b...");
    request_redraw(app);
}

static void cancel_request(struct background *app)
{
    if (app->provider_fd >= 0) {
        close(app->provider_fd);
        app->provider_fd = -1;
    }
    if (app->request_active) {
        app->request_active = false;
        app->provider_connecting = false;
        app->response_error = false;
        snprintf(app->status, sizeof(app->status), "Request cancelled.");
        if (app->configured) {
            request_redraw(app);
        }
    }
}

static void clear_conversation(struct background *app)
{
    cancel_request(app);
    app->prompt[0] = '\0';
    app->submitted_prompt[0] = '\0';
    app->response[0] = '\0';
    app->status[0] = '\0';
    app->has_submission = false;
    app->response_error = false;
    request_redraw(app);
}

static void provider_failure(struct background *app, const char *message)
{
    if (app->provider_fd >= 0) {
        close(app->provider_fd);
        app->provider_fd = -1;
    }
    app->request_active = false;
    app->provider_connecting = false;
    app->response_error = true;
    snprintf(app->status, sizeof(app->status), "%s", message);
    request_redraw(app);
}

static void provider_read_ready(struct background *app)
{
    for (;;) {
        unsigned char *target;
        size_t remaining;
        ssize_t count;
        if (app->response_header_received < sizeof(app->response_header)) {
            target = app->response_header + app->response_header_received;
            remaining = sizeof(app->response_header) - app->response_header_received;
        } else {
            target = (unsigned char *)app->response + app->response_received;
            remaining = app->response_length - app->response_received;
        }
        count = read(app->provider_fd, target, remaining);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            provider_failure(app, "Local model connection ended unexpectedly.");
            return;
        }
        if (app->response_header_received < sizeof(app->response_header)) {
            app->response_header_received += (size_t)count;
            if (app->response_header_received == sizeof(app->response_header)) {
                app->response_length =
                    ((size_t)app->response_header[1] << 24) |
                    ((size_t)app->response_header[2] << 16) |
                    ((size_t)app->response_header[3] << 8) |
                    (size_t)app->response_header[4];
                if (app->response_header[0] > 1 || app->response_length == 0 ||
                    app->response_length >= sizeof(app->response)) {
                    provider_failure(app, "Invalid local model response frame.");
                    return;
                }
            }
            continue;
        }
        app->response_received += (size_t)count;
        if (app->response_received < app->response_length) {
            continue;
        }
        app->response[app->response_length] = '\0';
        app->request_active = false;
        app->provider_connecting = false;
        close(app->provider_fd);
        app->provider_fd = -1;
        if (app->response_header[0] == 0) {
            snprintf(app->status, sizeof(app->status), "Model: qwen3:0.6b");
        } else {
            app->response_error = true;
            snprintf(app->status, sizeof(app->status), "Local model: %.220s",
                     app->response);
            app->response[0] = '\0';
        }
        request_redraw(app);
        return;
    }
}

static void provider_handle_event(struct background *app, short events)
{
    if (app->provider_fd < 0 || !app->request_active) {
        return;
    }
    if (app->provider_connecting &&
        (events & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) != 0) {
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(app->provider_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                       &error_length) < 0 || socket_error != 0) {
            provider_failure(app, socket_error != 0 ? strerror(socket_error)
                                                    : "Cannot connect to local model.");
            return;
        }
        app->provider_connecting = false;
    }
    if (app->provider_connecting) {
        return;
    }
    if ((events & POLLOUT) != 0) {
        while (app->request_sent < app->request_length) {
            ssize_t count = send(app->provider_fd,
                                 app->request_frame + app->request_sent,
                                 app->request_length - app->request_sent,
                                 MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (count <= 0) {
                provider_failure(app, "Cannot send prompt to local model.");
                return;
            }
            app->request_sent += (size_t)count;
        }
    }
    if ((events & POLLIN) != 0) {
        provider_read_ready(app);
        return;
    }
    if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        provider_failure(app, "Local model connection failed.");
    }
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    struct background *app = data;
    xkb_keycode_t keycode = key + 8;
    xkb_keysym_t keysym;
    char text[64];
    int text_length;
    size_t prompt_length;
    (void)keyboard;
    (void)serial;
    (void)time;

    if (app->xkb_state == NULL) {
        return;
    }
    xkb_state_update_key(app->xkb_state, keycode,
                         state == WL_KEYBOARD_KEY_STATE_PRESSED
                             ? XKB_KEY_DOWN : XKB_KEY_UP);
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !app->assistant_open ||
        !app->keyboard_focus) {
        return;
    }
    keysym = xkb_state_key_get_one_sym(app->xkb_state, keycode);
    if (keysym == XKB_KEY_Escape) {
        if (app->request_active) {
            cancel_request(app);
        } else {
            app->assistant_open = false;
            request_redraw(app);
        }
        return;
    }
    if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
        submit_prompt(app);
        return;
    }
    prompt_length = strlen(app->prompt);
    if (keysym == XKB_KEY_BackSpace) {
        if (prompt_length > 0) {
            do {
                prompt_length--;
            } while (prompt_length > 0 &&
                     ((unsigned char)app->prompt[prompt_length] & 0xc0) == 0x80);
            app->prompt[prompt_length] = '\0';
            request_redraw(app);
        }
        return;
    }
    text_length = xkb_state_key_get_utf8(app->xkb_state, keycode,
                                         text, sizeof(text));
    if (text_length <= 0 || (size_t)text_length >=
                                sizeof(app->prompt) - prompt_length) {
        return;
    }
    memcpy(app->prompt + prompt_length, text, (size_t)text_length);
    app->prompt[prompt_length + (size_t)text_length] = '\0';
    request_redraw(app);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    struct background *app = data;
    (void)keyboard;
    (void)serial;
    if (app->xkb_state != NULL) {
        xkb_state_update_mask(app->xkb_state, depressed, latched, locked,
                              0, 0, group);
    }
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
};

static void destroy_keyboard(struct background *app)
{
    if (app->keyboard != NULL) {
        wl_keyboard_destroy(app->keyboard);
        app->keyboard = NULL;
    }
    if (app->xkb_state != NULL) {
        xkb_state_unref(app->xkb_state);
        app->xkb_state = NULL;
    }
    if (app->xkb_keymap != NULL) {
        xkb_keymap_unref(app->xkb_keymap);
        app->xkb_keymap = NULL;
    }
    app->keyboard_focus = false;
}

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
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 &&
        app->keyboard == NULL) {
        app->keyboard = wl_seat_get_keyboard(seat);
        if (app->keyboard == NULL ||
            wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app) < 0) {
            fprintf(stderr, "BACKGROUND_WARNING: create Wayland keyboard\n");
            destroy_keyboard(app);
        }
    } else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0 &&
               app->keyboard != NULL) {
        destroy_keyboard(app);
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

static struct desktop_layout calculate_layout(struct background *app)
{
    struct desktop_layout layout = {0};
    double width = app->width;
    double height = app->height;
    double width_scale = width / 1440.0;
    double height_scale = height / 900.0;
    double panel_bottom;
    double available_width;

    layout.scale = clamp(width_scale < height_scale ? width_scale : height_scale,
                         0.75, 1.35);
    layout.taskbar_height = clamp(64.0 * layout.scale, 48.0, 72.0);
    layout.taskbar_y = height - layout.taskbar_height;
    layout.menu_width = clamp(126.0 * layout.scale, 100.0, 150.0);
    layout.menu_height = clamp(42.0 * layout.scale, 36.0, 48.0);
    layout.menu_x = (width - layout.menu_width) / 2.0;
    layout.menu_y = layout.taskbar_y +
                    (layout.taskbar_height - layout.menu_height) / 2.0;
    layout.pin_gap = clamp(9.0 * layout.scale, 5.0, 12.0);
    layout.pin_size = clamp(32.0 * layout.scale, 24.0, 38.0);
    available_width = width - (layout.menu_x + layout.menu_width +
                               layout.pin_gap) - 16.0;
    if (layout.pin_size * 3.0 + layout.pin_gap * 2.0 > available_width) {
        layout.pin_size = (available_width - layout.pin_gap * 2.0) / 3.0;
    }

    layout.panel_width = clamp(width * 0.82, 320.0, 620.0);
    if (layout.panel_width > width - 32.0) {
        layout.panel_width = width - 32.0;
    }
    layout.panel_height = clamp(height * 0.44, 320.0, 460.0);
    layout.panel_x = (width - layout.panel_width) / 2.0;
    panel_bottom = layout.taskbar_y - 12.0;
    if (layout.panel_height > panel_bottom - 16.0) {
        layout.panel_height = panel_bottom - 16.0;
    }
    layout.panel_y = panel_bottom - layout.panel_height;
    layout.prompt_x = layout.panel_x + 18.0;
    layout.prompt_y = layout.panel_y + layout.panel_height - 66.0;
    layout.prompt_width = layout.panel_width - 110.0;
    layout.prompt_height = 46.0;
    layout.submit_x = layout.prompt_x + layout.prompt_width + 10.0;
    layout.submit_width = 64.0;
    return layout;
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

static void draw_pin_icon(cairo_t *context, double x, double y, double size,
                          unsigned int icon)
{
    double inset = size * 0.23;
    double center_x = x + size / 2.0;
    double center_y = y + size / 2.0;

    cairo_set_source_rgb(context, 0.15, 0.23, 0.21);
    cairo_rectangle(context, x, y, size, size);
    cairo_fill(context);
    cairo_set_source_rgb(context, 0.77, 0.84, 0.75);
    cairo_set_line_width(context, 1.4);
    if (icon == 0) {
        cairo_rectangle(context, x + inset, y + inset, size - inset * 2.0,
                        size - inset * 2.0);
        cairo_stroke(context);
        cairo_move_to(context, x + inset * 1.5, center_y);
        cairo_line_to(context, x + size - inset * 1.5, center_y);
        cairo_stroke(context);
    } else if (icon == 1) {
        cairo_arc(context, center_x, center_y, size * 0.29, 0.0, 6.28318530718);
        cairo_stroke(context);
        cairo_move_to(context, center_x - size * 0.29, center_y);
        cairo_line_to(context, center_x + size * 0.29, center_y);
        cairo_move_to(context, center_x, center_y - size * 0.29);
        cairo_line_to(context, center_x, center_y + size * 0.29);
        cairo_stroke(context);
    } else {
        cairo_rectangle(context, x + inset, y + inset, size - inset * 2.0,
                        size - inset * 2.0);
        cairo_stroke(context);
        cairo_move_to(context, x + inset * 1.5, y + center_y - y - size * 0.08);
        cairo_line_to(context, x + size - inset * 1.5,
                      y + center_y - y - size * 0.08);
        cairo_move_to(context, x + inset * 1.5, y + size * 0.62);
        cairo_line_to(context, x + size - inset * 1.5, y + size * 0.62);
        cairo_stroke(context);
    }
}

static void draw_clipped_text(cairo_t *context, double x, double baseline,
                              double width, double size, const char *text,
                              bool bold, double red, double green, double blue)
{
    cairo_save(context);
    cairo_rectangle(context, x, baseline - size, width, size * 1.5);
    cairo_clip(context);
    draw_text(context, x, baseline, size, text, bold, red, green, blue);
    cairo_restore(context);
}

static void draw_wrapped_text(cairo_t *context, double x, double baseline,
                              double width, double size, const char *text,
                              bool bold, double red, double green, double blue,
                              unsigned int max_lines)
{
    char line[1024] = {0};
    size_t line_length = 0;
    unsigned int lines_drawn = 0;
    const char *cursor = text;

    if (max_lines == 0) {
        return;
    }
    cairo_save(context);
    cairo_rectangle(context, x, baseline - size, width,
                    (size * 1.4) * max_lines + size);
    cairo_clip(context);
    cairo_select_font_face(context, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, size);
    while (*cursor != '\0') {
        const char *word;
        size_t word_length;
        cairo_text_extents_t extents;
        while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' ||
               *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        word = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\n' &&
               *cursor != '\r' && *cursor != '\t') {
            cursor++;
        }
        word_length = (size_t)(cursor - word);
        if (word_length >= sizeof(line)) {
            word_length = sizeof(line) - 1;
        }
        if (line_length != 0) {
            line[line_length] = ' ';
            memcpy(line + line_length + 1, word, word_length);
            line[line_length + word_length + 1] = '\0';
        } else {
            memcpy(line, word, word_length);
            line[word_length] = '\0';
        }
        cairo_text_extents(context, line, &extents);
        if (line_length != 0 && extents.x_advance > width) {
            line[line_length] = '\0';
            draw_text(context, x, baseline + lines_drawn * size * 1.4, size,
                      line, bold, red, green, blue);
            lines_drawn++;
            if (lines_drawn >= max_lines) {
                cairo_restore(context);
                return;
            }
            memcpy(line, word, word_length);
            line[word_length] = '\0';
            line_length = word_length;
        } else {
            line_length += word_length + (line_length != 0 ? 1 : 0);
        }
    }
    if (line_length != 0 && lines_drawn < max_lines) {
        draw_text(context, x, baseline + lines_drawn * size * 1.4, size,
                  line, bold, red, green, blue);
    }
    cairo_restore(context);
}

static void render_desktop(struct background *app,
                           struct background_buffer *frame)
{
    cairo_t *context = cairo_create(frame->surface);
    double width = app->width;
    double height = app->height;
    struct desktop_layout layout = calculate_layout(app);
    cairo_pattern_t *background = cairo_pattern_create_linear(0, 0, width,
                                                               height);

    cairo_pattern_add_color_stop_rgb(background, 0.0, 0.105, 0.165, 0.153);
    cairo_pattern_add_color_stop_rgb(background, 0.58, 0.132, 0.209, 0.190);
    cairo_pattern_add_color_stop_rgb(background, 1.0, 0.160, 0.235, 0.207);
    cairo_set_source(context, background);
    cairo_paint(context);
    cairo_pattern_destroy(background);

    cairo_set_source_rgba(context, 0.90, 0.91, 0.83, 0.045);
    cairo_set_line_width(context, 1.0);
    cairo_move_to(context, width * 0.70, 0);
        cairo_line_to(context, width * 0.70, layout.taskbar_y - 1.0);
    cairo_stroke(context);

    cairo_set_source_rgb(context, 0.075, 0.116, 0.108);
        cairo_rectangle(context, 0, layout.taskbar_y, width, layout.taskbar_height);
    cairo_fill(context);
    cairo_set_source_rgb(context, 0.31, 0.46, 0.39);
        cairo_rectangle(context, 0, layout.taskbar_y, width, 1.0);
    cairo_fill(context);

        cairo_set_source_rgb(context, app->assistant_open ? 0.21 : 0.12,
                    app->assistant_open ? 0.31 : 0.19,
                    app->assistant_open ? 0.26 : 0.22);
        cairo_rectangle(context, layout.menu_x, layout.menu_y, layout.menu_width,
                  layout.menu_height);
    cairo_fill(context);
        cairo_set_source_rgb(context, 0.70, 0.79, 0.68);
        cairo_arc(context, layout.menu_x + 17.0 * layout.scale,
            layout.menu_y + layout.menu_height / 2.0,
            7.0 * layout.scale, 0.0, 6.28318530718);
    cairo_fill(context);
        draw_text(context, layout.menu_x + 31.0 * layout.scale,
            layout.menu_y + layout.menu_height * 0.65, 14.0 * layout.scale,
            "LUCIA", true, 0.93, 0.94, 0.88);

        if (layout.pin_size >= 18.0) {
         double pins_x = layout.menu_x + layout.menu_width + layout.pin_gap;
         draw_text(context, pins_x, layout.taskbar_y + 12.0 * layout.scale,
                7.0 * layout.scale, "DEMO", true, 0.67, 0.77, 0.68);
         for (unsigned int icon = 0; icon < 3; icon++) {
             draw_pin_icon(context, pins_x + icon *
                     (layout.pin_size + layout.pin_gap),
                     layout.taskbar_y +
                     (layout.taskbar_height - layout.pin_size) / 2.0 +
                     3.0 * layout.scale,
                     layout.pin_size, icon);
         }
        }

        if (app->assistant_open) {
         double conversation_top = layout.panel_y + 74.0;
         cairo_set_source_rgb(context, 0.075, 0.112, 0.103);
         cairo_rectangle(context, layout.panel_x, layout.panel_y,
                   layout.panel_width, layout.panel_height);
         cairo_fill(context);
         cairo_set_source_rgb(context, 0.34, 0.49, 0.41);
         cairo_rectangle(context, layout.panel_x, layout.panel_y,
                   layout.panel_width, 1.0);
         cairo_fill(context);
         draw_text(context, layout.panel_x + 20.0,
                layout.panel_y + 32.0, 17.0, "LUCIA", true,
                0.93, 0.94, 0.88);
         draw_text(context, layout.panel_x + 20.0,
                layout.panel_y + 52.0, 10.0, "Conversation", false,
                0.66, 0.76, 0.69);
          draw_text(context, layout.panel_x + layout.panel_width - 98.0,
              layout.panel_y + 32.0, 10.0, "CLEAR", true,
              0.68, 0.78, 0.69);
         cairo_set_source_rgb(context, 0.68, 0.78, 0.69);
         cairo_set_line_width(context, 1.8);
         cairo_move_to(context, layout.panel_x + layout.panel_width - 30.0,
                 layout.panel_y + 20.0);
         cairo_line_to(context, layout.panel_x + layout.panel_width - 18.0,
                 layout.panel_y + 32.0);
         cairo_move_to(context, layout.panel_x + layout.panel_width - 18.0,
                 layout.panel_y + 20.0);
         cairo_line_to(context, layout.panel_x + layout.panel_width - 30.0,
                 layout.panel_y + 32.0);
         cairo_stroke(context);
         if (app->has_submission) {
             draw_text(context, layout.panel_x + 20.0, conversation_top,
                 10.0, "You", true, 0.67, 0.79, 0.68);
             draw_clipped_text(context, layout.panel_x + 20.0,
                      conversation_top + 20.0,
                      layout.panel_width - 40.0, 13.0,
                      app->submitted_prompt, false,
                      0.91, 0.93, 0.87);
             if (app->status[0] != '\0') {
                 draw_clipped_text(context, layout.panel_x + 20.0,
                         conversation_top + 52.0,
                         layout.panel_width - 40.0, 10.0,
                         app->status, true,
                         app->response_error ? 0.91 : 0.67,
                         app->response_error ? 0.52 : 0.79,
                         app->response_error ? 0.46 : 0.68);
             }
             if (app->response[0] != '\0') {
                 draw_wrapped_text(context, layout.panel_x + 20.0,
                         conversation_top + 78.0,
                         layout.panel_width - 40.0, 12.0,
                     app->response, false, 0.91, 0.93, 0.87, 10);
             }
         } else {
             draw_text(context, layout.panel_x + 20.0, conversation_top,
                 13.0, "Hi. What would you like to explore?", false,
                 0.91, 0.93, 0.87);
             draw_text(context, layout.panel_x + 20.0,
                 conversation_top + 23.0, 11.0,
                 "Replies use the local qwen3:0.6b model.", false,
                 0.68, 0.78, 0.71);
         }
         cairo_set_source_rgb(context, 0.12, 0.17, 0.15);
         cairo_rectangle(context, layout.prompt_x, layout.prompt_y,
                   layout.prompt_width, layout.prompt_height);
         cairo_fill(context);
         cairo_set_source_rgb(context, 0.37, 0.52, 0.43);
         cairo_rectangle(context, layout.prompt_x, layout.prompt_y,
                   layout.prompt_width, layout.prompt_height);
         cairo_stroke(context);
         draw_clipped_text(context, layout.prompt_x + 12.0,
                     layout.prompt_y + 28.0,
                     layout.prompt_width - 24.0, 12.0,
                     app->prompt[0] == '\0' ? "Type a message..." : app->prompt,
                     false, app->prompt[0] == '\0' ? 0.53 : 0.93,
                     app->prompt[0] == '\0' ? 0.62 : 0.94,
                     app->prompt[0] == '\0' ? 0.57 : 0.88);
         cairo_set_source_rgb(context, app->request_active ? 0.35 : 0.20,
                              app->request_active ? 0.27 : 0.34,
                              app->request_active ? 0.23 : 0.27);
         cairo_rectangle(context, layout.submit_x, layout.prompt_y,
                         layout.submit_width, layout.prompt_height);
         cairo_fill(context);
         draw_text(context, layout.submit_x + 9.0,
                   layout.prompt_y + 28.0, 10.0,
                   app->request_active ? "CANCEL" : "SEND", true,
                   0.93, 0.94, 0.88);
         printf("LUCIA_ASSISTANT_PANEL: state=open x=%.0f y=%.0f width=%.0f height=%.0f\n",
             layout.panel_x, layout.panel_y, layout.panel_width,
             layout.panel_height);
         printf("LUCIA_ASSISTANT_PROMPT: x=%.0f y=%.0f width=%.0f height=%.0f\n",
             layout.prompt_x, layout.prompt_y, layout.prompt_width,
             layout.prompt_height);
        }

    cairo_destroy(context);
    cairo_surface_flush(frame->surface);
    printf("LUCIA_TASKBAR_LAYOUT: edge=bottom x=0 y=%u width=%u height=%.0f\n",
            (unsigned int)layout.taskbar_y, app->width, layout.taskbar_height);
        printf("LUCIA_MENU_LAYOUT: center x=%.0f y=%.0f width=%.0f height=%.0f\n",
            layout.menu_x, layout.menu_y, layout.menu_width, layout.menu_height);
        printf("LUCIA_DEMO_PINS: count=3 launch=disabled\n");
        printf("LUCIA_SHELL_ELEMENTS: identity=LUCIA menu=center apps=demo-pinned conversation=%s status=none\n",
            app->assistant_open ? "open" : "closed");
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

static void request_redraw(struct background *app)
{
    struct background_buffer *frame;
    if (!app->configured) {
        return;
    }
    frame = create_background_buffer(app);
    if (frame == NULL) {
        app->running = false;
        return;
    }
    activate_buffer(app, frame);
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
    if (app->provider_fd >= 0) {
        close(app->provider_fd);
        app->provider_fd = -1;
    }
    if (app->frame_callback != NULL) {
        wl_callback_destroy(app->frame_callback);
    }
    while (app->buffers != NULL) {
        release_buffer(app->buffers);
    }
    if (app->pointer != NULL) {
        wl_pointer_destroy(app->pointer);
    }
    destroy_keyboard(app);
    if (app->seat != NULL) {
        wl_seat_destroy(app->seat);
    }
    if (app->xkb_context != NULL) {
        xkb_context_unref(app->xkb_context);
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
    struct background app = {.running = true, .provider_fd = -1};
    int result = EXIT_FAILURE;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && strcmp(argv[1], "--smoke") == 0) {
        app.smoke = true;
        app.assistant_open = true;
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
    app.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (app.xkb_context == NULL) {
        fprintf(stderr, "BACKGROUND_WARNING: create xkb context; keyboard input unavailable\n");
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
        struct pollfd descriptors[2];
        nfds_t descriptor_count = 1;
        int poll_result;
        if (wl_display_dispatch_pending(app.display) < 0) {
            fprintf(stderr, "BACKGROUND_ERROR: Wayland pending dispatch failed\n");
            goto shutdown;
        }
        if (wl_display_flush(app.display) < 0 && errno != EAGAIN) {
            fprintf(stderr, "BACKGROUND_ERROR: Wayland flush failed\n");
            goto shutdown;
        }
        descriptors[0].fd = wl_display_get_fd(app.display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        if (app.provider_fd >= 0 && app.request_active) {
            descriptors[1].fd = app.provider_fd;
            descriptors[1].events = app.provider_connecting ||
                                            app.request_sent < app.request_length
                                        ? POLLOUT : POLLIN;
            descriptors[1].revents = 0;
            descriptor_count = 2;
        }
        poll_result = poll(descriptors, descriptor_count, -1);
        if (poll_result < 0 && errno == EINTR) {
            continue;
        }
        if (poll_result < 0) {
            fprintf(stderr, "BACKGROUND_ERROR: event poll failed: %s\n",
                    strerror(errno));
            goto shutdown;
        }
        if ((descriptors[0].revents & POLLIN) != 0 &&
            wl_display_dispatch(app.display) < 0) {
            fprintf(stderr, "BACKGROUND_ERROR: Wayland dispatch failed\n");
            goto shutdown;
        }
        if (descriptor_count == 2 && descriptors[1].fd == app.provider_fd &&
            descriptors[1].revents != 0) {
            provider_handle_event(&app, descriptors[1].revents);
        }
    }
    result = app.frame_presented ? EXIT_SUCCESS : EXIT_FAILURE;

shutdown:
    destroy_background(&app);
    printf("LIFECYCLE: shutdown\n");
    return result;
}