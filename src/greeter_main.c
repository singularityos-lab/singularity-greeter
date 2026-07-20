#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <poll.h>
#include <pwd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "loginui.h"

/* ── model ──────────────────────────────────────────────────────────────── */

struct g_user {
    char username[256];
    char realname[256];
    cairo_surface_t *avatar;
    cairo_surface_t *bg;
};
static struct g_user users[32];
static int n_users = 0, sel_user = 0;

struct g_session {
    char name[128];
    char exec[1024];
};
static struct g_session sessions[32];
static int n_sessions = 0, sel_session = 0;

static char current_user[256] = "";
static cairo_surface_t *os_logo = NULL;

static char password[1024];
static size_t password_len = 0;
static char pending_password[1024];
static char status_text[128] = "";
static bool auth_is_pin = false; /* SintyOS: the credential is a PIN, not a Unix password */
static bool recovery_mode = false; /* --recovery: forgotten-PIN escape hatch, launched as root by sinty-greetd-launch */
static int recovery_step = 0;      /* 0 = enter recovery code, 1 = enter new PIN */
static char recovery_code[256] = "";
static bool status_error = false;
static bool awaiting_auth = false;

static int greetd_fd = -1;
static bool preview = false;
static bool running = true;

struct rect { double x, y, w, h; bool valid; };
static struct rect user_hit[32];
static struct rect session_hit;
static struct rect power_hit;
static struct rect power_item[3];
static bool power_open = false;
static double ptr_x, ptr_y;

/* ── wayland globals ────────────────────────────────────────────────────── */

struct g_output {
    struct wl_output *wl_output;
    uint32_t name;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    uint32_t width, height;
    bool configured;
    struct g_output *next;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct xdg_wm_base *wm_base;
static struct g_output *outputs;

static struct wl_surface *pv_surface;
static struct xdg_surface *pv_xsurf;
static struct xdg_toplevel *pv_top;
static int pv_w = 1000, pv_h = 640;
static bool pv_configured = false;

static struct xkb_context *xkb_context;
static struct xkb_keymap *xkb_keymap;
static struct xkb_state *xkb_state;

static void render_all(void);

/* ── chrome + render ────────────────────────────────────────────────────── */

static const char *power_labels[3] = { "Sleep", "Restart", "Shut Down" };

static void draw_power_glyph(cairo_t *cr, double cx, double cy, double r) {
    cairo_set_source_rgba(cr, 0.96, 0.96, 0.98, 0.96);
    cairo_set_line_width(cr, 2);
    cairo_arc(cr, cx, cy, r * 0.42, -60 * (3.14159265 / 180.0), 240 * (3.14159265 / 180.0));
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - r * 0.55);
    cairo_line_to(cr, cx, cy - r * 0.05);
    cairo_stroke(cr);
}

static void render_surface(struct wl_surface *surface, int w, int h) {
    cairo_t *cr;
    struct loginui_buffer *b = loginui_create_buffer(shm, w, h, &cr);
    if (!b) return;

    struct g_user *u = (n_users > 0) ? &users[sel_user] : NULL;

    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    char tbuf[32], dbuf[64];
    strftime(tbuf, sizeof tbuf, "%H:%M", &tm);
    strftime(dbuf, sizeof dbuf, "%A, %B %e", &tm);

    LoginUiState st = {0};
    st.width = w;
    st.height = h;
    st.username = u ? (u->realname[0] ? u->realname : u->username) : "user";
    st.time_str = tbuf;
    st.date_str = dbuf;
    st.avatar = u ? u->avatar : NULL;
    st.background = u ? u->bg : NULL;
    st.password_dots = (int)password_len;
    st.status_text = status_text[0] ? status_text : NULL;
    st.status_error = status_error;
    if (recovery_mode)
        st.auth_label = recovery_step == 0 ? "Recovery code" : "New PIN";
    else
        st.auth_label = auth_is_pin ? "PIN" : "Password";

    double cx, cy, cw, ch;
    loginui_render(cr, &st, &cx, &cy, &cw, &ch);

    for (int i = 0; i < 32; i++) user_hit[i].valid = false;
    if (n_users > 1) {
        double a = 44, gap = 16;
        double total = n_users * a + (n_users - 1) * gap;
        double x = (w - total) / 2.0, y = 36;
        for (int i = 0; i < n_users; i++) {
            double ux = x + i * (a + gap);
            user_hit[i] = (struct rect){ ux, y, a, a, true };
            cairo_save(cr);
            cairo_arc(cr, ux + a / 2, y + a / 2, a / 2, 0, 2 * 3.14159265);
            cairo_clip(cr);
            if (users[i].avatar) {
                int aw = cairo_image_surface_get_width(users[i].avatar);
                double sc = a / (double)aw;
                cairo_translate(cr, ux, y);
                cairo_scale(cr, sc, sc);
                cairo_set_source_surface(cr, users[i].avatar, 0, 0);
                cairo_paint(cr);
            } else {
                cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
                cairo_paint(cr);
            }
            cairo_restore(cr);
            if (i == sel_user) {
                cairo_arc(cr, ux + a / 2, y + a / 2, a / 2 + 2, 0, 2 * 3.14159265);
                cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
                cairo_set_line_width(cr, 2);
                cairo_stroke(cr);
            }
        }
    }

    session_hit.valid = false;
    if (n_sessions > 0) {
        const char *sname = sessions[sel_session].name;
        int tw, th;
        loginui_text_size(cr, "Sans 12", sname, &tw, &th);
        double cw2 = tw + 28, chh = 28;
        double sx = w - 16 - cw2, sy = h - 16 - chh;
        loginui_rounded_rect(cr, sx, sy, cw2, chh, 14);
        cairo_set_source_rgba(cr, 0.13, 0.13, 0.14, 0.92);
        cairo_fill(cr);
        loginui_text(cr, "Sans 12", sname, sx + cw2 / 2.0, sy + (chh - th) / 2.0, 1, 0.9, 0.9, 0.93);
        session_hit = (struct rect){ sx, sy, cw2, chh, true };
    }
    (void)cx; (void)cy; (void)cw; (void)ch;

    if (os_logo) {
        int lw = cairo_image_surface_get_width(os_logo);
        int lh = cairo_image_surface_get_height(os_logo);
        cairo_set_source_surface(cr, os_logo, (w - lw) / 2.0, h - 18 - lh);
        cairo_paint_with_alpha(cr, 0.85);
    }

    double pr = 18, pcx = w - 36, pcy = 36;
    loginui_rounded_rect(cr, pcx - pr, pcy - pr, 2 * pr, 2 * pr, pr);
    cairo_set_source_rgba(cr, 0.13, 0.13, 0.14, 0.92);
    cairo_fill(cr);
    draw_power_glyph(cr, pcx, pcy, pr);
    power_hit = (struct rect){ pcx - pr, pcy - pr, 2 * pr, 2 * pr, true };
    for (int i = 0; i < 3; i++) power_item[i].valid = false;
    if (power_open) {
        double mw = 160, mh = 36, mx = w - 16 - mw, my = pcy + pr + 8;
        loginui_rounded_rect(cr, mx, my, mw, mh * 3 + 8, 12);
        cairo_set_source_rgba(cr, 0.176, 0.176, 0.176, 0.98);
        cairo_fill(cr);
        for (int i = 0; i < 3; i++) {
            double iy = my + 4 + i * mh;
            power_item[i] = (struct rect){ mx, iy, mw, mh, true };
            loginui_text(cr, "Sans 12", power_labels[i], mx + 14, iy + (mh - 16) / 2.0, 0, 0.92, 0.92, 0.94);
        }
    }

    cairo_destroy(cr);
    wl_surface_attach(surface, b->wl_buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, w, h);
    wl_surface_commit(surface);
}

static void render_all(void) {
    if (preview) {
        if (pv_configured) render_surface(pv_surface, pv_w, pv_h);
        return;
    }
    for (struct g_output *o = outputs; o; o = o->next)
        if (o->configured) render_surface(o->surface, (int)o->width, (int)o->height);
}

/* ── greetd IPC ─────────────────────────────────────────────────────────── */

static bool greetd_send(const char *json) {
    uint32_t len = (uint32_t)strlen(json);
    if (write(greetd_fd, &len, 4) != 4) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t wn = write(greetd_fd, json + off, len - off);
        if (wn <= 0) return false;
        off += (size_t)wn;
    }
    return true;
}

static void greetd_send_object(JsonBuilder *b) {
    json_builder_end_object(b);
    JsonGenerator *g = json_generator_new();
    json_generator_set_root(g, json_builder_get_root(b));
    gsize jlen = 0;
    char *json = json_generator_to_data(g, &jlen);
    greetd_send(json);
    g_free(json);
    g_object_unref(g);
    g_object_unref(b);
}

static void greetd_create_session(const char *user) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "create_session");
    json_builder_set_member_name(b, "username"); json_builder_add_string_value(b, user);
    greetd_send_object(b);
}

static void greetd_post_response(const char *response) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "post_auth_message_response");
    json_builder_set_member_name(b, "response");
    if (response) json_builder_add_string_value(b, response);
    else json_builder_add_null_value(b);
    greetd_send_object(b);
}

static void greetd_cancel_session(void) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "cancel_session");
    greetd_send_object(b);
}

static void greetd_start_session(const char *exec) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "start_session");
    json_builder_set_member_name(b, "cmd");
    json_builder_begin_array(b);
    char *copy = g_strdup(exec);
    char *save = NULL;
    for (char *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
        if (tok[0]) json_builder_add_string_value(b, tok);
    g_free(copy);
    json_builder_end_array(b);
    greetd_send_object(b);
}

static bool read_exact(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

/* recovery escape hatch: the greeter stays UNPRIVILEGED. It sends {uid, code, new PIN}
 * to sinty-recoverd (root daemon) over its unix socket; the daemon runs the privileged
 * `sintykey recover`. Auth is the recovery code; a wrong one is rejected by sintykey. */
static void run_recover(const char *code, const char *newpin) {
    struct passwd *pw = (n_users > 0) ? getpwnam(users[sel_user].username) : NULL;
    if (!pw) { snprintf(status_text, sizeof status_text, "User not found"); status_error = true; recovery_step = 0; render_all(); return; }
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    /* bound the round-trip: this runs on the Wayland event-loop thread, so a hung
     * recoverd would freeze the whole greeter (no login for anyone). A timeout reads
     * as failure (fail-closed) and returns control. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    if (s >= 0) {
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, "/run/sinty-recoverd.sock", sizeof a.sun_path - 1);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        if (s >= 0) close(s);
        snprintf(status_text, sizeof status_text, "Recovery service unavailable");
        status_error = true; recovery_step = 0; render_all(); return;
    }
    dprintf(s, "%u\n%s\n%s\n", (unsigned)pw->pw_uid, code, newpin);
    char resp[32] = {0};
    ssize_t n = read(s, resp, sizeof resp - 1);
    close(s);
    if (n >= 3 && strncmp(resp, "OK\n", 3) == 0) {
        snprintf(status_text, sizeof status_text, "PIN reset. Log in with your new PIN.");
        status_error = false;
        recovery_mode = false; recovery_step = 0; /* back to the normal login, same session */
    } else if (n > 0 && strncmp(resp, "BLOCKED", 7) == 0) {
        snprintf(status_text, sizeof status_text, "Too many attempts. Try again later.");
        status_error = true; recovery_step = 0;
    } else {
        snprintf(status_text, sizeof status_text, "Recovery failed. Check your recovery code.");
        status_error = true; recovery_step = 0;
    }
    render_all();
}

static void submit_recovery(void) {
    if (recovery_step == 0) {
        g_strlcpy(recovery_code, password, sizeof recovery_code);
        memset(password, 0, sizeof password); password_len = 0;
        recovery_step = 1;
        status_text[0] = '\0'; status_error = false;
        render_all();
        return;
    }
    char newpin[1024]; g_strlcpy(newpin, password, sizeof newpin);
    memset(password, 0, sizeof password); password_len = 0;
    run_recover(recovery_code, newpin);
    memset(recovery_code, 0, sizeof recovery_code);
    memset(newpin, 0, sizeof newpin);
}

static void submit_password(void) {
    if (password_len == 0) return;
    if (recovery_mode) { submit_recovery(); return; }
    if (n_users == 0) return;
    g_strlcpy(pending_password, password, sizeof pending_password);
    memset(password, 0, sizeof password);
    password_len = 0;
    snprintf(status_text, sizeof status_text, "%s", "Authenticating…");
    status_error = false;
    render_all();
    wl_display_flush(display);
    if (preview) { snprintf(status_text, sizeof status_text, "Preview"); render_all(); return; }
    awaiting_auth = true;
    greetd_create_session(users[sel_user].username);
}

static void greetd_handle(void) {
    uint32_t len = 0;
    if (!read_exact(greetd_fd, &len, 4)) { running = false; return; }
    if (len == 0 || len > 65536) return;
    char *payload = malloc(len + 1);
    if (!payload) return;
    if (!read_exact(greetd_fd, payload, len)) { free(payload); running = false; return; }
    payload[len] = '\0';

    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, payload, len, NULL)) {
        JsonNode *rn = json_parser_get_root(p);
        if (rn && JSON_NODE_HOLDS_OBJECT(rn)) {
            JsonObject *root = json_node_get_object(rn);
            const char *type = json_object_has_member(root, "type")
                ? json_object_get_string_member(root, "type") : "";
            if (g_strcmp0(type, "auth_message") == 0) {
                const char *amt = json_object_has_member(root, "auth_message_type")
                    ? json_object_get_string_member(root, "auth_message_type") : "";
                if (g_strcmp0(amt, "error") == 0 || g_strcmp0(amt, "info") == 0) {
                    const char *m = json_object_has_member(root, "auth_message")
                        ? json_object_get_string_member(root, "auth_message") : "";
                    snprintf(status_text, sizeof status_text, "%s", m);
                    status_error = (g_strcmp0(amt, "error") == 0);
                    render_all();
                } else {
                    greetd_post_response(pending_password);
                    memset(pending_password, 0, sizeof pending_password);
                }
            } else if (g_strcmp0(type, "success") == 0) {
                if (awaiting_auth) {
                    awaiting_auth = false;
                    if (n_sessions > 0) greetd_start_session(sessions[sel_session].exec);
                    wl_display_flush(display);
                    running = false;
                }
            } else if (g_strcmp0(type, "error") == 0) {
                const char *desc = json_object_has_member(root, "description")
                    ? json_object_get_string_member(root, "description") : "Authentication failed";
                snprintf(status_text, sizeof status_text, "%s", desc);
                status_error = true;
                awaiting_auth = false;
                greetd_cancel_session();
                memset(password, 0, sizeof password);
                memset(pending_password, 0, sizeof pending_password);
                password_len = 0;
                render_all();
            }
        }
    }
    g_object_unref(p);
    free(payload);
}

static bool greetd_connect(void) {
    const char *path = getenv("GREETD_SOCK");
    if (!path) return false;
    greetd_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (greetd_fd < 0) return false;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(greetd_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(greetd_fd); greetd_fd = -1; return false;
    }
    return true;
}

static void power_action(int which) {
    if (preview) return;
    /* Drive power via login1 on the system bus. Under sinit there is no systemd, so
     * `systemctl reboot/poweroff` was a no-op; sinty-logind serves login1 as root and
     * maps Reboot/PowerOff/Suspend to sinit reboot/poweroff and the kernel. */
    const char *method = which == 0 ? "Suspend"
                       : which == 1 ? "Reboot"
                                    : "PowerOff";
    char *cmd = g_strdup_printf(
        "gdbus call --system --dest org.freedesktop.login1 "
        "--object-path /org/freedesktop/login1 "
        "--method org.freedesktop.login1.Manager.%s false", method);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
}

/* ── users / sessions / assets ──────────────────────────────────────────── */

static cairo_surface_t *load_user_bg(const char *user) {
    cairo_surface_t *bg = NULL;

    char acc[600];
    snprintf(acc, sizeof acc, "/var/lib/AccountsService/users/%s", user);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, acc, G_KEY_FILE_NONE, NULL)) {
        char *b = g_key_file_get_string(kf, "com.singularity.Desktop", "Background", NULL);
        if (b && b[0]) bg = loginui_load_wallpaper(b, 960);
        g_free(b);
    }
    g_key_file_free(kf);

    if (!bg && current_user[0] && g_strcmp0(user, current_user) == 0) {
        GSettingsSchemaSource *src = g_settings_schema_source_get_default();
        if (src && g_settings_schema_source_lookup(src, "dev.sinty.desktop", TRUE)) {
            GSettings *s = g_settings_new("dev.sinty.desktop");
            char *uri = g_settings_get_string(s, "background-picture-uri");
            if (uri && uri[0]) bg = loginui_load_wallpaper(uri, 960);
            g_free(uri);
            g_object_unref(s);
        }
    }

    if (!bg) {
        const char *fb[] = {
            "/opt/local/share/backgrounds/singularity/default.png",
            "/usr/local/share/backgrounds/singularity/default.png",
            "/usr/share/backgrounds/singularity/default.png",
            NULL
        };
        for (int i = 0; fb[i] && !bg; i++)
            if (access(fb[i], R_OK) == 0) bg = loginui_load_wallpaper(fb[i], 960);
    }
    return bg;
}

static cairo_surface_t *load_user_avatar(const char *user) {
    char p[600];
    snprintf(p, sizeof p, "/var/lib/AccountsService/icons/%s", user);
    if (access(p, R_OK) == 0) return loginui_load_avatar(p, 92);
    snprintf(p, sizeof p, "/home/%s/.face", user);
    if (access(p, R_OK) == 0) return loginui_load_avatar(p, 92);
    return NULL;
}

static void load_users(void) {
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) != NULL && n_users < 32) {
        if (pw->pw_uid < 1000 || pw->pw_uid >= 65000) continue;
        if (!pw->pw_shell || strstr(pw->pw_shell, "nologin") || strstr(pw->pw_shell, "/false"))
            continue;
        struct g_user *u = &users[n_users++];
        snprintf(u->username, sizeof u->username, "%s", pw->pw_name);
        if (pw->pw_gecos && pw->pw_gecos[0]) {
            snprintf(u->realname, sizeof u->realname, "%s", pw->pw_gecos);
            char *comma = strchr(u->realname, ','); if (comma) *comma = '\0';
        }
        if (u->realname[0] == '\0') snprintf(u->realname, sizeof u->realname, "%s", u->username);
        u->avatar = load_user_avatar(u->username);
        u->bg = load_user_bg(u->username);
    }
    endpwent();
}

static void load_sessions(void) {
    const char *dirs[] = {
        "/opt/local/share/wayland-sessions",
        "/usr/local/share/wayland-sessions",
        "/usr/share/wayland-sessions",
        NULL
    };
    for (int i = 0; dirs[i] && n_sessions < 32; i++) {
        GDir *d = g_dir_open(dirs[i], 0, NULL);
        if (!d) continue;
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL && n_sessions < 32) {
            if (!g_str_has_suffix(name, ".desktop")) continue;
            char *full = g_build_filename(dirs[i], name, NULL);
            GKeyFile *kf = g_key_file_new();
            if (g_key_file_load_from_file(kf, full, G_KEY_FILE_NONE, NULL)) {
                char *exec = g_key_file_get_string(kf, "Desktop Entry", "Exec", NULL);
                char *nm = g_key_file_get_string(kf, "Desktop Entry", "Name", NULL);
                if (exec && exec[0]) {
                    struct g_session *s = &sessions[n_sessions];
                    snprintf(s->exec, sizeof s->exec, "%s", exec);
                    snprintf(s->name, sizeof s->name, "%s", (nm && nm[0]) ? nm : name);
                    if (strstr(name, "singularity") || strstr(exec, "singularity")) sel_session = n_sessions;
                    n_sessions++;
                }
                g_free(exec); g_free(nm);
            }
            g_key_file_free(kf);
            g_free(full);
        }
        g_dir_close(d);
    }
}

static bool try_logo_file(const char *name) {
    if (!name || !name[0]) return false;
    const char *tpl[] = {
        "/opt/local/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/local/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/share/pixmaps/%s.svg",
        "/usr/share/pixmaps/%s.png",
        "/usr/share/icons/hicolor/256x256/apps/%s.png",
        NULL
    };
    for (int i = 0; tpl[i]; i++) {
        char p[1024];
        snprintf(p, sizeof p, tpl[i], name);
        if (access(p, R_OK) == 0) { os_logo = loginui_load_image(p, -1, 13); if (os_logo) return true; }
    }
    return false;
}

static void find_os_logo(void) {
    char logo[128] = "", id[128] = "";
    char *content = NULL;
    if (g_file_get_contents("/etc/os-release", &content, NULL, NULL)) {
        char **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *v = NULL, *dst = NULL; size_t n = 0;
            if (g_str_has_prefix(lines[i], "LOGO=")) { v = lines[i] + 5; dst = logo; n = sizeof logo; }
            else if (g_str_has_prefix(lines[i], "ID=")) { v = lines[i] + 3; dst = id; n = sizeof id; }
            if (v) {
                char *t = g_strdup(v);
                g_strstrip(t);
                if (t[0] == '"') memmove(t, t + 1, strlen(t));
                char *q = strchr(t, '"'); if (q) *q = '\0';
                snprintf(dst, n, "%s", t);
                g_free(t);
            }
        }
        g_strfreev(lines);
        g_free(content);
    }
    auth_is_pin = (id[0] != '\0' && g_str_has_prefix(id, "sinty"));
    if (try_logo_file(logo)) return;
    if (try_logo_file(id)) return;
    try_logo_file("emblem-singularity");
}

/* ── pointer ────────────────────────────────────────────────────────────── */

static bool in_rect(struct rect *r) {
    return r->valid && ptr_x >= r->x && ptr_x <= r->x + r->w &&
           ptr_y >= r->y && ptr_y <= r->y + r->h;
}

static void pt_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
    ptr_x = wl_fixed_to_double(x); ptr_y = wl_fixed_to_double(y);
}
static void pt_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {}
static void pt_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    ptr_x = wl_fixed_to_double(x); ptr_y = wl_fixed_to_double(y);
}
static void pt_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
                      uint32_t button, uint32_t state) {
    if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED) return;
    if (power_open) {
        for (int i = 0; i < 3; i++) if (in_rect(&power_item[i])) { power_open = false; power_action(i); render_all(); return; }
        if (!in_rect(&power_hit)) { power_open = false; render_all(); return; }
    }
    if (in_rect(&power_hit)) { power_open = !power_open; render_all(); return; }
    if (in_rect(&session_hit) && n_sessions > 1) { sel_session = (sel_session + 1) % n_sessions; render_all(); return; }
    for (int i = 0; i < n_users; i++) if (in_rect(&user_hit[i])) {
        sel_user = i; status_text[0] = '\0'; memset(password, 0, sizeof password); password_len = 0; render_all(); return;
    }
}
static void pt_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void pt_frame(void *d, struct wl_pointer *p) {}
static void pt_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void pt_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void pt_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pt_enter, .leave = pt_leave, .motion = pt_motion, .button = pt_button,
    .axis = pt_axis, .frame = pt_frame, .axis_source = pt_axis_source,
    .axis_stop = pt_axis_stop, .axis_discrete = pt_axis_discrete,
};

/* ── keyboard ───────────────────────────────────────────────────────────── */

static void kb_keymap(void *d, struct wl_keyboard *kb, uint32_t format, int fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    if (xkb_keymap) xkb_keymap_unref(xkb_keymap);
    if (xkb_state) xkb_state_unref(xkb_state);
    xkb_keymap = xkb_keymap_new_from_string(xkb_context, map,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (xkb_keymap) xkb_state = xkb_state_new(xkb_keymap);
}
static void kb_enter(void *d, struct wl_keyboard *kb, uint32_t s, struct wl_surface *sf, struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *kb, uint32_t s, struct wl_surface *sf) {}
static void kb_modifiers(void *d, struct wl_keyboard *kb, uint32_t s,
                         uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) {
    if (xkb_state) xkb_state_update_mask(xkb_state, dep, lat, lck, 0, 0, grp);
}
static void kb_repeat(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay) {}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
    if (!xkb_state || state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    xkb_keycode_t kc = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state, kc);

    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        submit_password();
        return;
    case XKB_KEY_BackSpace:
        if (password_len > 0) password[--password_len] = '\0';
        status_text[0] = '\0';
        render_all();
        return;
    case XKB_KEY_Escape:
        if (recovery_mode) { recovery_mode = false; recovery_step = 0; }
        memset(password, 0, sizeof password); password_len = 0; status_text[0] = '\0';
        power_open = false;
        render_all();
        return;
    case XKB_KEY_Tab:
        if (n_users > 1) { sel_user = (sel_user + 1) % n_users; memset(password, 0, sizeof password); password_len = 0; status_text[0] = '\0'; render_all(); }
        return;
    case XKB_KEY_F1:
        /* Forgot PIN? Enter the in-process recovery mode (the greeter stays unprivileged;
         * the actual recover goes to sinty-recoverd). SintyOS only. */
        if (!recovery_mode && auth_is_pin) {
            recovery_mode = true;
            recovery_step = 0;
            memset(password, 0, sizeof password); password_len = 0;
            status_text[0] = '\0'; status_error = false;
            render_all();
        }
        return;
    default: break;
    }

    char buf[8];
    int n = xkb_state_key_get_utf8(xkb_state, kc, buf, sizeof buf);
    if (n > 0 && (unsigned char)buf[0] >= 0x20 && password_len + n < sizeof password - 1) {
        memcpy(password + password_len, buf, n);
        password_len += n;
        password[password_len] = '\0';
        status_text[0] = '\0';
        render_all();
    }
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat,
};

/* ── surfaces ───────────────────────────────────────────────────────────── */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h) {
    struct g_output *o = data;
    o->width = w; o->height = h; o->configured = true;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    render_surface(o->surface, (int)w, (int)h);
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) { running = false; }
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_configure, .closed = layer_closed,
};

static void create_layer_surface(struct g_output *o) {
    if (o->layer_surface || !layer_shell) return;
    o->surface = wl_compositor_create_surface(compositor);
    o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, o->surface, o->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "greeter");
    zwlr_layer_surface_v1_set_anchor(o->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(o->layer_surface, &layer_surface_listener, o);
    wl_surface_commit(o->surface);
}

static void xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    xdg_surface_ack_configure(xs, serial);
    pv_configured = true;
    render_surface(pv_surface, pv_w, pv_h);
}
static const struct xdg_surface_listener xdg_surface_listener = { .configure = xdg_surface_configure };

static void xdg_top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
    if (w > 0) pv_w = w;
    if (h > 0) pv_h = h;
}
static void xdg_top_close(void *data, struct xdg_toplevel *t) { running = false; }
static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_top_configure, .close = xdg_top_close,
};

static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void create_preview_window(void) {
    pv_surface = wl_compositor_create_surface(compositor);
    pv_xsurf = xdg_wm_base_get_xdg_surface(wm_base, pv_surface);
    xdg_surface_add_listener(pv_xsurf, &xdg_surface_listener, NULL);
    pv_top = xdg_surface_get_toplevel(pv_xsurf);
    xdg_toplevel_add_listener(pv_top, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(pv_top, "Singularity Greeter (preview)");
    wl_surface_commit(pv_surface);
}

/* ── registry ───────────────────────────────────────────────────────────── */

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version) {
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        keyboard = wl_seat_get_keyboard(seat);
        if (keyboard) wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
        pointer = wl_seat_get_pointer(seat);
        if (pointer) wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        struct g_output *o = calloc(1, sizeof(*o));
        o->name = name;
        o->wl_output = wl_registry_bind(reg, name, &wl_output_interface, version < 3 ? version : 3);
        o->next = outputs;
        outputs = o;
    }
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {
    struct g_output **pp = &outputs;
    while (*pp) {
        if ((*pp)->name == name) {
            struct g_output *dead = *pp;
            *pp = dead->next;
            if (dead->layer_surface) zwlr_layer_surface_v1_destroy(dead->layer_surface);
            if (dead->surface) wl_surface_destroy(dead->surface);
            if (dead->wl_output) wl_output_destroy(dead->wl_output);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}
static const struct wl_registry_listener registry_listener = { reg_global, reg_remove };

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--preview") == 0) preview = true;
        if (strcmp(argv[i], "--recovery") == 0) recovery_mode = true;
    }
    if (getenv("SINGULARITY_GREETER_PREVIEW")) preview = true;

    /* Initialize fontconfig explicitly before any cairo text/font use. cairo
     * otherwise auto-inits it on first use and fontconfig prints the "using
     * without calling FcInit()" warning to stderr on every launch. */
    FcInit();

    struct passwd *me = getpwuid(getuid());
    if (me && me->pw_name) snprintf(current_user, sizeof current_user, "%s", me->pw_name);

    load_users();
    load_sessions();
    find_os_logo();

    if (!preview && !recovery_mode && !greetd_connect()) {
        fprintf(stderr, "greeter: cannot connect to greetd ($GREETD_SOCK)\n");
        return 1;
    }

    display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "greeter: cannot connect to Wayland display\n"); return 1; }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !seat || (!preview && !layer_shell) || (preview && !wm_base)) {
        fprintf(stderr, "greeter: compositor missing required globals\n");
        return 1;
    }

    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    if (preview) {
        create_preview_window();
    } else {
        for (struct g_output *o = outputs; o; o = o->next) create_layer_surface(o);
    }
    wl_display_roundtrip(display);

    int wfd = wl_display_get_fd(display);
    int last_min = -1;
    while (running) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);

        struct pollfd pfds[2];
        pfds[0].fd = wfd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = greetd_fd; pfds[1].events = POLLIN; pfds[1].revents = 0;

        int pr = poll(pfds, 2, 1000);

        if (pr > 0 && (pfds[0].revents & POLLIN)) wl_display_read_events(display);
        else wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);

        if (greetd_fd >= 0 && pr > 0 && (pfds[1].revents & (POLLIN | POLLHUP))) greetd_handle();

        time_t now = time(NULL);
        struct tm tm; localtime_r(&now, &tm);
        if (tm.tm_min != last_min) { last_min = tm.tm_min; render_all(); }
    }

    wl_display_roundtrip(display);
    return 0;
}
