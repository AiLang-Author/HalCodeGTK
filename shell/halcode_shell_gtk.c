/* halcode_shell_gtk — thin GTK3 chrome + blit for HalCode9000.
 *
 * Native window, menu, status. Kernel owns every document window
 * (AppDesk MDI) and writes CAD-style:
 *   meta.bin   int32 w, h, pitch
 *   frame.raw  BGRA, pitch * h
 *   gen.txt    generation integer
 *
 * Input will be @halcode/shell JSON. Do not use cmd.txt.
 *
 *   HALCODE_APP_STATE=/tmp/halcode_app ./halcode_shell_gtk
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#include <gtk/gtk.h>
#include <cairo.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[512];
static char path_meta[600], path_frame[600], path_gen[600];
static char path_ptr[600], path_cmd[600], path_keys[600];

static GtkWidget *g_win;
static GtkWidget *g_da;
static GtkWidget *g_status;
static cairo_surface_t *g_surf;
static int g_fw, g_fh, g_last_gen = -1;

static void paths_init(const char *dir) {
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    snprintf(path_meta, sizeof path_meta, "%s/meta.bin", dir);
    snprintf(path_frame, sizeof path_frame, "%s/frame.raw", dir);
    snprintf(path_gen, sizeof path_gen, "%s/gen.txt", dir);
    snprintf(path_ptr, sizeof path_ptr, "%s/ptr.txt", dir);
    snprintf(path_cmd, sizeof path_cmd, "%s/cmd.txt", dir);
    snprintf(path_keys, sizeof path_keys, "%s/keys.txt", dir);
}

static void write_line(const char *path, const char *s) {
    char tmp[640];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs(s, f);
    fputc('\n', f);
    fclose(f);
    rename(tmp, path);
}

static void write_cmd(const char *s) { write_line(path_cmd, s); }

static void write_ptr(const char *kind, double wx, double wy) {
    GtkAllocation a;
    int fx, fy;
    char buf[64];
    if (!g_da || g_fw < 16 || g_fh < 16) return;
    gtk_widget_get_allocation(g_da, &a);
    if (a.width < 1 || a.height < 1) return;
    fx = (int)(wx * (double)g_fw / (double)a.width);
    fy = (int)(wy * (double)g_fh / (double)a.height);
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx >= g_fw) fx = g_fw - 1;
    if (fy >= g_fh) fy = g_fh - 1;
    snprintf(buf, sizeof buf, "%s %d %d", kind, fx, fy);
    write_line(path_ptr, buf);
}

static int read_gen(void) {
    char buf[32];
    int fd = open(path_gen, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return atoi(buf);
}

static int load_frame(void) {
    int fd = open(path_meta, O_RDONLY);
    if (fd < 0) return -1;
    int32_t hdr[3];
    if (read(fd, hdr, 12) != 12) {
        close(fd);
        return -1;
    }
    close(fd);
    int w = hdr[0], h = hdr[1], pitch = hdr[2];
    if (w < 16 || h < 16 || pitch < w * 4) return -1;
    size_t sz = (size_t)pitch * (size_t)h;
    uint8_t *pix = (uint8_t *)malloc(sz);
    if (!pix) return -1;
    fd = open(path_frame, O_RDONLY);
    if (fd < 0) {
        free(pix);
        return -1;
    }
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, pix + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < sz) {
        free(pix);
        return -1;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        free(pix);
        return -1;
    }
    unsigned char *dst = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    int y, x;
    for (y = 0; y < h; y++) {
        uint8_t *srow = pix + (size_t)y * (size_t)pitch;
        uint32_t *drow = (uint32_t *)(dst + (size_t)y * (size_t)stride);
        for (x = 0; x < w; x++) {
            uint8_t b = srow[x * 4 + 0];
            uint8_t g = srow[x * 4 + 1];
            uint8_t r = srow[x * 4 + 2];
            drow[x] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(surf);
    free(pix);
    if (g_surf) cairo_surface_destroy(g_surf);
    g_surf = surf;
    g_fw = w;
    g_fh = h;
    return 0;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    cairo_set_source_rgb(cr, 14.0 / 255.0, 18.0 / 255.0, 24.0 / 255.0);
    cairo_paint(cr);
    if (!g_surf) {
        cairo_set_source_rgb(cr, 0.55, 0.62, 0.72);
        cairo_move_to(cr, 24, 40);
        cairo_show_text(cr, "waiting for kernel frame (meta.bin / frame.raw / gen.txt)");
        return FALSE;
    }
    int sw = cairo_image_surface_get_width(g_surf);
    int sh = cairo_image_surface_get_height(g_surf);
    double sx = (a.width > 0) ? (double)a.width / (double)sw : 1.0;
    double sy = (a.height > 0) ? (double)a.height / (double)sh : 1.0;
    cairo_save(cr);
    cairo_scale(cr, sx, sy);
    cairo_set_source_surface(cr, g_surf, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static gboolean poll_tick(gpointer data) {
    (void)data;
    int g = read_gen();
    if (g != g_last_gen && g >= 0) {
        if (load_frame() == 0) {
            g_last_gen = g;
            if (g_da) gtk_widget_queue_draw(g_da);
            if (g_status && g_fw > 0) {
                char line[192];
                snprintf(line, sizeof line, "frame %d  %dx%d", g, g_fw, g_fh);
                gtk_label_set_text(GTK_LABEL(g_status), line);
            }
        }
    }
    return TRUE;
}

static void on_quit(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    write_cmd("quit");
    gtk_main_quit();
}

static gboolean on_button(GtkWidget *w, GdkEventButton *e, gpointer data) {
    (void)data;
    gtk_widget_grab_focus(w);
    if (e->type == GDK_BUTTON_PRESS)
        write_ptr("d", e->x, e->y);
    else if (e->type == GDK_BUTTON_RELEASE)
        write_ptr("u", e->x, e->y);
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer data) {
    (void)w;
    (void)data;
    if (e->state & GDK_BUTTON1_MASK)
        write_ptr("m", e->x, e->y);
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer data) {
    char buf[32];
    guint32 u;
    (void)w;
    (void)data;
    if ((e->state & GDK_CONTROL_MASK) &&
        (e->keyval == GDK_KEY_q || e->keyval == GDK_KEY_Q)) {
        write_cmd("quit");
        gtk_main_quit();
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Return || e->keyval == GDK_KEY_KP_Enter) {
        write_line(path_keys, "r");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_BackSpace) {
        write_line(path_keys, "b");
        return TRUE;
    }
    u = gdk_keyval_to_unicode(e->keyval);
    if (u >= 32 && u < 127) {
        snprintf(buf, sizeof buf, "c %u", (unsigned)u);
        write_line(path_keys, buf);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : getenv("HALCODE_APP_STATE");
    if (!dir || !dir[0]) dir = "/tmp/halcode_app";
    paths_init(dir);

    gtk_init(&argc, &argv);

    g_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_win), "HalCode9000");
    gtk_window_set_default_size(GTK_WINDOW(g_win), 1100, 720);
    g_signal_connect(g_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(g_win, "key-press-event", G_CALLBACK(on_key), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g_win), root);

    GtkWidget *mbar = gtk_menu_bar_new();
    GtkWidget *file = gtk_menu_item_new_with_label("File");
    GtkWidget *fm = gtk_menu_new();
    GtkWidget *quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(fm), quit);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fm);
    gtk_menu_shell_append(GTK_MENU_SHELL(mbar), file);
    gtk_box_pack_start(GTK_BOX(root), mbar, FALSE, FALSE, 0);

    g_da = gtk_drawing_area_new();
    gtk_widget_set_hexpand(g_da, TRUE);
    gtk_widget_set_vexpand(g_da, TRUE);
    gtk_widget_set_size_request(g_da, 1100, 720);
    gtk_widget_set_can_focus(g_da, TRUE);
    gtk_widget_add_events(g_da,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_BUTTON1_MOTION_MASK |
        GDK_KEY_PRESS_MASK);
    g_signal_connect(g_da, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(g_da, "button-press-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(g_da, "button-release-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(g_da, "motion-notify-event", G_CALLBACK(on_motion), NULL);
    gtk_box_pack_start(GTK_BOX(root), g_da, TRUE, TRUE, 0);

    g_status = gtk_label_new("waiting for kernel");
    gtk_widget_set_name(g_status, "statbar");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0.0);
    gtk_box_pack_start(GTK_BOX(root), g_status, FALSE, FALSE, 0);

    g_timeout_add(16, poll_tick, NULL);
    gtk_widget_show_all(g_win);
    gtk_widget_grab_focus(g_da);
    gtk_main();

    if (g_surf) cairo_surface_destroy(g_surf);
    return 0;
}
