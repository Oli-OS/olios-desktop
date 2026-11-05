#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <json-c/json.h>
#include <errno.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_IPC_PAYLOAD (1024U * 1024U)
#define SESSION_COMMAND "/usr/local/libexec/oli-desktop-session"

typedef struct {
    char *username;
    char *display_name;
} Account;

typedef struct {
    int socket_fd;
    gboolean waiting_for_response;
    GPtrArray *accounts;
    GtkWidget *window;
    GtkWidget *profile_name;
    GtkWidget *profile_selector;
    GtkWidget *auth_prompt;
    GtkWidget *password_entry;
    GtkWidget *submit_button;
    GtkWidget *status_label;
} Login;

static void
account_free(gpointer data)
{
    Account *account = data;
    g_free(account->username);
    g_free(account->display_name);
    g_free(account);
}

static int
read_exact(int fd, void *buffer, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t result = read(fd, (char *)buffer + offset, length - offset);
        if (result == 0)
            return -1;
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        offset += (size_t)result;
    }
    return 0;
}

static int
write_exact(int fd, const void *buffer, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t result = send(fd, (const char *)buffer + offset,
                              length - offset, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        offset += (size_t)result;
    }
    return 0;
}

static int
send_json(int fd, struct json_object *message)
{
    const char *payload = json_object_to_json_string_ext(
        message, JSON_C_TO_STRING_PLAIN);
    size_t payload_length = strlen(payload);
    if (payload_length == 0 || payload_length > MAX_IPC_PAYLOAD)
        return -1;

    uint32_t length = (uint32_t)payload_length;
    if (write_exact(fd, &length, sizeof(length)) < 0)
        return -1;
    return write_exact(fd, payload, payload_length);
}

static struct json_object *
receive_json(int fd)
{
    uint32_t length = 0;
    if (read_exact(fd, &length, sizeof(length)) < 0 || length == 0 ||
        length > MAX_IPC_PAYLOAD)
        return NULL;

    char *payload = g_malloc((size_t)length + 1);
    if (read_exact(fd, payload, length) < 0) {
        g_free(payload);
        return NULL;
    }
    payload[length] = '\0';

    struct json_tokener *tokener = json_tokener_new();
    struct json_object *message = json_tokener_parse_ex(tokener, payload,
                                                         (int)length);
    enum json_tokener_error error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    g_free(payload);
    if (error != json_tokener_success || message == NULL ||
        !json_object_is_type(message, json_type_object)) {
        if (message != NULL)
            json_object_put(message);
        return NULL;
    }
    return message;
}

static struct json_object *
request(const char *type)
{
    struct json_object *message = json_object_new_object();
    json_object_object_add(message, "type", json_object_new_string(type));
    return message;
}

static int
expect_string(struct json_object *message, const char *key, const char *value)
{
    struct json_object *field = NULL;
    return json_object_object_get_ex(message, key, &field) &&
           json_object_is_type(field, json_type_string) &&
           strcmp(json_object_get_string(field), value) == 0;
}

static int
send_auth_response(int fd, const char *response)
{
    struct json_object *message = request("post_auth_message_response");
    if (response != NULL)
        json_object_object_add(message, "response",
                               json_object_new_string(response));
    int result = send_json(fd, message);
    json_object_put(message);
    return result;
}

static int
connect_greetd(void)
{
    const char *socket_path = getenv("GREETD_SOCK");
    if (socket_path == NULL || socket_path[0] == '\0' ||
        strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int
test_server(int fd)
{
    struct json_object *message = receive_json(fd);
    if (message == NULL || !expect_string(message, "type", "create_session") ||
        !expect_string(message, "username", "test-user")) {
        if (message != NULL)
            json_object_put(message);
        return 1;
    }
    json_object_put(message);

    message = json_object_new_object();
    json_object_object_add(message, "type", json_object_new_string("auth_message"));
    json_object_object_add(message, "auth_message_type",
                           json_object_new_string("secret"));
    json_object_object_add(message, "auth_message",
                           json_object_new_string("Password:"));
    if (send_json(fd, message) < 0) {
        json_object_put(message);
        return 1;
    }
    json_object_put(message);

    message = receive_json(fd);
    if (message == NULL ||
        !expect_string(message, "type", "post_auth_message_response") ||
        !expect_string(message, "response", "test-secret")) {
        if (message != NULL)
            json_object_put(message);
        return 1;
    }
    json_object_put(message);

    message = request("success");
    if (send_json(fd, message) < 0) {
        json_object_put(message);
        return 1;
    }
    json_object_put(message);

    message = receive_json(fd);
    struct json_object *command = NULL;
    struct json_object *environment = NULL;
    int valid_session = message != NULL &&
        expect_string(message, "type", "start_session") &&
        json_object_object_get_ex(message, "cmd", &command) &&
        json_object_is_type(command, json_type_array) &&
        json_object_array_length(command) == 1 &&
        strcmp(json_object_get_string(json_object_array_get_idx(command, 0)),
               SESSION_COMMAND) == 0 &&
        json_object_object_get_ex(message, "env", &environment) &&
        json_object_is_type(environment, json_type_array) &&
        json_object_array_length(environment) == 0;
    if (message != NULL)
        json_object_put(message);
    if (!valid_session)
        return 1;

    message = request("success");
    int result = send_json(fd, message) < 0;
    json_object_put(message);
    return result;
}

static int
run_ipc_self_test(void)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
        return 1;

    pid_t child = fork();
    if (child < 0) {
        close(sockets[0]);
        close(sockets[1]);
        return 1;
    }
    if (child == 0) {
        close(sockets[0]);
        int result = test_server(sockets[1]);
        close(sockets[1]);
        _exit(result);
    }

    close(sockets[1]);
    struct json_object *message = request("create_session");
    json_object_object_add(message, "username",
                           json_object_new_string("test-user"));
    int result = send_json(sockets[0], message) < 0;
    json_object_put(message);

    message = result ? NULL : receive_json(sockets[0]);
    result = result || message == NULL ||
        !expect_string(message, "type", "auth_message") ||
        !expect_string(message, "auth_message_type", "secret");
    if (message != NULL)
        json_object_put(message);

    if (!result && send_auth_response(sockets[0], "test-secret") < 0)
        result = 1;
    message = result ? NULL : receive_json(sockets[0]);
    result = result || message == NULL ||
        !expect_string(message, "type", "success");
    if (message != NULL)
        json_object_put(message);

    if (!result) {
        message = request("start_session");
        struct json_object *command = json_object_new_array();
        struct json_object *environment = json_object_new_array();
        json_object_array_add(command,
                              json_object_new_string(SESSION_COMMAND));
        json_object_object_add(message, "cmd", command);
        json_object_object_add(message, "env", environment);
        if (send_json(sockets[0], message) < 0)
            result = 1;
        json_object_put(message);
    }
    message = result ? NULL : receive_json(sockets[0]);
    result = result || message == NULL ||
        !expect_string(message, "type", "success");
    if (message != NULL)
        json_object_put(message);
    close(sockets[0]);

    int child_status = 0;
    if (waitpid(child, &child_status, 0) < 0 ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        result = 1;
    if (result) {
        fprintf(stderr, "greetd IPC self-test failed\n");
        return 1;
    }
    puts("GREETD_IPC_SELF_TEST_OK");
    return 0;
}

static gboolean
is_human_account(const struct passwd *entry)
{
    if (entry->pw_uid < 1000 || entry->pw_dir == NULL ||
        entry->pw_dir[0] != '/' || entry->pw_shell == NULL)
        return FALSE;
    return strcmp(entry->pw_shell, "/usr/sbin/nologin") != 0 &&
           strcmp(entry->pw_shell, "/sbin/nologin") != 0 &&
           strcmp(entry->pw_shell, "/bin/false") != 0 &&
           strcmp(entry->pw_shell, "/usr/bin/false") != 0;
}

static void
load_accounts(Login *login)
{
    login->accounts = g_ptr_array_new_with_free_func(account_free);
    setpwent();
    struct passwd *entry;
    while ((entry = getpwent()) != NULL) {
        if (!is_human_account(entry))
            continue;
        Account *account = g_new0(Account, 1);
        account->username = g_strdup(entry->pw_name);
        const char *gecos = entry->pw_gecos != NULL ? entry->pw_gecos : "";
        const char *comma = strchr(gecos, ',');
        account->display_name = comma != NULL
            ? g_strndup(gecos, (gsize)(comma - gecos))
            : g_strdup(gecos);
        g_strstrip(account->display_name);
        if (account->display_name[0] == '\0') {
            g_free(account->display_name);
            account->display_name = g_strdup(account->username);
        }
        g_ptr_array_add(login->accounts, account);
    }
    endpwent();
}

static Account *
selected_account(Login *login)
{
    if (login->accounts->len == 0)
        return NULL;
    if (login->profile_selector == NULL)
        return g_ptr_array_index(login->accounts, 0);

    const char *selected_id = gtk_combo_box_get_active_id(
        GTK_COMBO_BOX(login->profile_selector));
    if (selected_id == NULL)
        return NULL;
    for (guint index = 0; index < login->accounts->len; index++) {
        Account *account = g_ptr_array_index(login->accounts, index);
        if (strcmp(account->username, selected_id) == 0)
            return account;
    }
    return NULL;
}

static void
show_status(Login *login, const char *message)
{
    gtk_label_set_text(GTK_LABEL(login->status_label), message);
    gtk_widget_set_visible(login->status_label, message[0] != '\0');
}

static void
close_greetd(Login *login)
{
    if (login->socket_fd >= 0) {
        close(login->socket_fd);
        login->socket_fd = -1;
    }
    login->waiting_for_response = FALSE;
}

static int
send_create_session(Login *login, const char *username)
{
    struct json_object *message = request("create_session");
    json_object_object_add(message, "username",
                           json_object_new_string(username));
    int result = send_json(login->socket_fd, message);
    json_object_put(message);
    return result;
}

static int
start_oli_session(Login *login)
{
    struct json_object *message = request("start_session");
    struct json_object *command = json_object_new_array();
    struct json_object *environment = json_object_new_array();
    json_object_array_add(command, json_object_new_string(SESSION_COMMAND));
    json_object_object_add(message, "cmd", command);
    json_object_object_add(message, "env", environment);
    int result = send_json(login->socket_fd, message);
    json_object_put(message);
    if (result < 0)
        return -1;

    message = receive_json(login->socket_fd);
    result = message != NULL && expect_string(message, "type", "success")
        ? 0 : -1;
    if (message != NULL)
        json_object_put(message);
    return result;
}

static void
submit_credentials(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    Login *login = user_data;
    gtk_widget_set_sensitive(login->submit_button, FALSE);
    gtk_widget_set_sensitive(login->password_entry, FALSE);
    show_status(login, "");

    if (login->socket_fd < 0) {
        Account *account = selected_account(login);
        if (account == NULL || (login->socket_fd = connect_greetd()) < 0 ||
            send_create_session(login, account->username) < 0) {
            close_greetd(login);
            show_status(login, "Unable to contact the login service.");
            goto ready;
        }
    } else if (login->waiting_for_response) {
        const char *entry_text = gtk_entry_get_text(
            GTK_ENTRY(login->password_entry));
        char *response = g_strdup(entry_text != NULL ? entry_text : "");
        gtk_entry_set_text(GTK_ENTRY(login->password_entry), "");
        if (send_auth_response(login->socket_fd, response) < 0) {
            memset(response, 0, strlen(response));
            g_free(response);
            close_greetd(login);
            show_status(login, "Unable to contact the login service.");
            goto ready;
        }
        memset(response, 0, strlen(response));
        g_free(response);
        login->waiting_for_response = FALSE;
    }

    for (;;) {
        struct json_object *message = receive_json(login->socket_fd);
        if (message == NULL) {
            close_greetd(login);
            show_status(login, "The login service returned an invalid response.");
            break;
        }

        if (expect_string(message, "type", "auth_message")) {
            struct json_object *message_type = NULL;
            struct json_object *prompt = NULL;
            gboolean has_type = json_object_object_get_ex(
                message, "auth_message_type", &message_type);
            gboolean has_prompt = json_object_object_get_ex(
                message, "auth_message", &prompt);
            const char *type = has_type ? json_object_get_string(message_type) : "";
            const char *text = has_prompt ? json_object_get_string(prompt) : "";

            if (strcmp(type, "info") == 0 || strcmp(type, "error") == 0) {
                show_status(login, text);
                json_object_put(message);
                if (send_auth_response(login->socket_fd, NULL) < 0) {
                    close_greetd(login);
                    show_status(login, "Unable to contact the login service.");
                    break;
                }
                continue;
            }
            if (strcmp(type, "visible") != 0 && strcmp(type, "secret") != 0) {
                json_object_put(message);
                close_greetd(login);
                show_status(login, "The login service requested an unsupported response.");
                break;
            }

            gtk_label_set_text(GTK_LABEL(login->auth_prompt), text);
            gtk_entry_set_placeholder_text(GTK_ENTRY(login->password_entry),
                strcmp(type, "secret") == 0 ? "Password" : "Response");
            gtk_entry_set_visibility(GTK_ENTRY(login->password_entry),
                                     strcmp(type, "secret") != 0);
            login->waiting_for_response = TRUE;
            json_object_put(message);
            gtk_widget_set_sensitive(login->password_entry, TRUE);
            gtk_widget_set_sensitive(login->submit_button, TRUE);
            gtk_widget_grab_focus(login->password_entry);
            return;
        }

        if (expect_string(message, "type", "success")) {
            json_object_put(message);
            if (start_oli_session(login) == 0) {
                close_greetd(login);
                gtk_main_quit();
                return;
            }
            close_greetd(login);
            show_status(login, "Authentication succeeded, but the desktop could not start.");
            break;
        }

        struct json_object *error_type = NULL;
        gboolean authentication_failed =
            expect_string(message, "type", "error") &&
            json_object_object_get_ex(message, "error_type", &error_type) &&
            json_object_is_type(error_type, json_type_string) &&
            strcmp(json_object_get_string(error_type), "auth_error") == 0;
        json_object_put(message);
        close_greetd(login);
        show_status(login, authentication_failed
            ? "Sign-in failed. Check your password and try again."
            : "The login service could not start this session.");
        break;
    }

ready:
    gtk_entry_set_text(GTK_ENTRY(login->password_entry), "");
    gtk_widget_set_sensitive(login->password_entry, TRUE);
    gtk_widget_set_sensitive(login->submit_button, TRUE);
    gtk_widget_grab_focus(login->password_entry);
}

static void
account_changed(GtkComboBox *selector, gpointer user_data)
{
    Login *login = user_data;
    Account *account = selected_account(login);
    if (account != NULL)
        gtk_label_set_text(GTK_LABEL(login->profile_name),
                           account->display_name);
    close_greetd(login);
    gtk_entry_set_text(GTK_ENTRY(login->password_entry), "");
    show_status(login, "");
    (void)selector;
}

static void
window_destroyed(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    Login *login = user_data;
    close_greetd(login);
    gtk_main_quit();
}

static gboolean
window_configured(GtkWidget *widget, GdkEventConfigure *event,
                  gpointer user_data)
{
    (void)event;
    g_signal_handlers_disconnect_by_func(
        widget, G_CALLBACK(window_configured), user_data);
    gtk_window_fullscreen(GTK_WINDOW(widget));
    return FALSE;
}

static void
load_style(const char *path)
{
    if (path == NULL || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return;
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (gtk_css_provider_load_from_path(provider, path, &error)) {
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    } else {
        g_warning("Unable to load greeter stylesheet: %s", error->message);
        g_clear_error(&error);
    }
    g_object_unref(provider);
}

static GtkWidget *
create_login_window(Login *login, const char *avatar_path)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(window, "login-window");
    gtk_window_set_title(GTK_WINDOW(window), "Oli Desktop");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(root, "login-root");
    gtk_container_add(GTK_CONTAINER(window), root);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_name(content, "login-content");
    gtk_widget_set_halign(content, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(content, 360, -1);
    gtk_box_pack_start(GTK_BOX(root), content, TRUE, TRUE, 0);

    GtkWidget *avatar = gtk_image_new_from_file(avatar_path);
    gtk_widget_set_name(avatar, "profile-avatar");
    gtk_widget_set_halign(avatar, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(avatar, 112, 112);
    gtk_box_pack_start(GTK_BOX(content), avatar, FALSE, FALSE, 0);

    login->profile_name = gtk_label_new("");
    gtk_widget_set_name(login->profile_name, "profile-name");
    gtk_label_set_ellipsize(GTK_LABEL(login->profile_name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(login->profile_name), 32);
    gtk_box_pack_start(GTK_BOX(content), login->profile_name, FALSE, FALSE, 0);

    if (login->accounts->len > 1) {
        login->profile_selector = gtk_combo_box_text_new();
        gtk_widget_set_name(login->profile_selector, "profile-selector");
        for (guint index = 0; index < login->accounts->len; index++) {
            Account *account = g_ptr_array_index(login->accounts, index);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(login->profile_selector),
                                      account->username, account->username);
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(login->profile_selector), 0);
        gtk_widget_set_halign(login->profile_selector, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(content), login->profile_selector,
                           FALSE, FALSE, 0);
        g_signal_connect(login->profile_selector, "changed",
                         G_CALLBACK(account_changed), login);
    }

    login->auth_prompt = gtk_label_new("Password");
    gtk_widget_set_name(login->auth_prompt, "auth-prompt");
    gtk_label_set_ellipsize(GTK_LABEL(login->auth_prompt), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(login->auth_prompt), 48);
    gtk_box_pack_start(GTK_BOX(content), login->auth_prompt, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(row, "password-row");
    gtk_box_pack_start(GTK_BOX(content), row, FALSE, FALSE, 0);

    login->password_entry = gtk_entry_new();
    gtk_widget_set_name(login->password_entry, "password-entry");
    gtk_entry_set_visibility(GTK_ENTRY(login->password_entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(login->password_entry),
                                GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_placeholder_text(GTK_ENTRY(login->password_entry), "Password");
    gtk_widget_set_hexpand(login->password_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(row), login->password_entry, TRUE, TRUE, 0);

    login->submit_button = gtk_button_new_from_icon_name(
        "go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_name(login->submit_button, "submit-button");
    gtk_widget_set_tooltip_text(login->submit_button, "Sign in");
    gtk_button_set_relief(GTK_BUTTON(login->submit_button), GTK_RELIEF_NONE);
    gtk_box_pack_start(GTK_BOX(row), login->submit_button, FALSE, FALSE, 0);

    login->status_label = gtk_label_new("");
    gtk_widget_set_name(login->status_label, "status-label");
    gtk_label_set_line_wrap(GTK_LABEL(login->status_label), TRUE);
    gtk_widget_set_visible(login->status_label, FALSE);
    gtk_box_pack_start(GTK_BOX(content), login->status_label, FALSE, FALSE, 0);

    Account *account = selected_account(login);
    if (account != NULL)
        gtk_label_set_text(GTK_LABEL(login->profile_name),
                           account->display_name);

    g_signal_connect(login->submit_button, "clicked",
                     G_CALLBACK(submit_credentials), login);
    g_signal_connect(login->password_entry, "activate",
                     G_CALLBACK(submit_credentials), login);
    g_signal_connect(window, "destroy", G_CALLBACK(window_destroyed), login);
    g_signal_connect(window, "configure-event",
                     G_CALLBACK(window_configured), login);
    return window;
}

int
main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return run_ipc_self_test();
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--self-test]\n", argv[0]);
        return 2;
    }

    gtk_init(&argc, &argv);
    Login login = {.socket_fd = -1};
    load_accounts(&login);
    if (login.accounts->len == 0) {
        fprintf(stderr, "No eligible user accounts are available.\n");
        g_ptr_array_free(login.accounts, TRUE);
        return 1;
    }

    const char *style_path = getenv("OLI_GREETER_STYLE");
    const char *avatar_path = getenv("OLI_GREETER_AVATAR");
    if (style_path == NULL)
        style_path = "/etc/oli-desktop/greeter.css";
    if (avatar_path == NULL)
        avatar_path = "/usr/local/share/oli-desktop/avatar.svg";
    load_style(style_path);
    login.window = create_login_window(&login, avatar_path);
    gtk_widget_show_all(login.window);
    gtk_widget_set_visible(login.status_label, FALSE);
    gtk_widget_grab_focus(login.password_entry);
    gtk_main();

    close_greetd(&login);
    g_ptr_array_free(login.accounts, TRUE);
    return 0;
}