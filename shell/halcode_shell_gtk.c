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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char g_dir[512];
static char path_meta[600], path_frame[600], path_gen[600];
static char path_ptr[600], path_cmd[600], path_keys[600];
static char path_go[600], path_prompt[600], path_reply[600], path_model[600];
static pid_t g_agent = -1;
static pid_t g_host = -1;
static pid_t g_prov = -1;
static time_t g_agent_t0;
static time_t g_agent_prog;
static time_t g_agent_term;
static off_t g_prog_n;
static time_t g_host_t0;
static time_t g_go_wait;
static char g_app_root[512];
static char path_fetch_providers[600];
static FILE *g_dbg;
static int g_tickn;
static int g_quitting;

static void dbg(const char *fmt, ...) {
    va_list ap;
    struct timespec ts;
    if (!g_dbg) {
        g_dbg = fopen("/tmp/halcode_app/debug.log", "a");
        if (!g_dbg) return;
        setvbuf(g_dbg, NULL, _IOLBF, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(g_dbg, "[gtk %.3f] ", ts.tv_sec + ts.tv_nsec / 1e9);
    va_start(ap, fmt);
    vfprintf(g_dbg, fmt, ap);
    va_end(ap);
    fputc('\n', g_dbg);
}

static GtkWidget *g_win;
static GtkWidget *g_da;
static GtkWidget *g_status;
static GtkWidget *g_ctxmenu;
static cairo_surface_t *g_surf;
static int g_fw, g_fh, g_last_gen = -1;
static long g_frame_ms;
static int g_size_w, g_size_h;

static void resolve_app_root(void) {
    const char *env;
    char exe[512];
    ssize_t n;
    char *slash;

    env = getenv("HALCODE_ROOT");
    if (env && env[0]) {
        snprintf(g_app_root, sizeof g_app_root, "%s", env);
        return;
    }
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            slash = strrchr(exe, '/');
            if (slash && strcmp(slash, "/shell") == 0)
                *slash = 0;
            snprintf(g_app_root, sizeof g_app_root, "%s", exe);
            return;
        }
    }
    snprintf(g_app_root, sizeof g_app_root, ".");
}

static void write_size_file(int w, int h) {
    char p[600];
    FILE *f;
    if (w < 640) w = 640;
    if (h < 400) h = 400;
    if (w == g_size_w && h == g_size_h) return;
    g_size_w = w;
    g_size_h = h;
    snprintf(p, sizeof p, "%s/size.txt", g_dir[0] ? g_dir : "/tmp/halcode_app");
    f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%d %d\n", w, h);
    fclose(f);
}

static gboolean on_da_configure(GtkWidget *w, GdkEventConfigure *e, gpointer data) {
    (void)w;
    (void)data;
    if (e && e->width >= 64 && e->height >= 64)
        write_size_file(e->width, e->height);
    return FALSE;
}

static void default_window_size(int *ww, int *wh) {
    GdkDisplay *dpy;
    GdkMonitor *mon;
    GdkRectangle r;
    *ww = 1100;
    *wh = 720;
    dpy = gdk_display_get_default();
    if (!dpy) return;
    mon = gdk_display_get_primary_monitor(dpy);
    if (!mon) {
        GdkScreen *sc = gdk_screen_get_default();
        if (sc) {
            *ww = gdk_screen_get_width(sc) * 9 / 10;
            *wh = gdk_screen_get_height(sc) * 9 / 10;
        }
        return;
    }
    gdk_monitor_get_workarea(mon, &r);
    *ww = r.width * 9 / 10;
    *wh = r.height * 9 / 10;
    if (*ww < 800) *ww = 800;
    if (*wh < 560) *wh = 560;
}

static void paths_init(const char *dir) {
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    snprintf(path_meta, sizeof path_meta, "%s/meta.bin", dir);
    snprintf(path_frame, sizeof path_frame, "%s/frame.raw", dir);
    snprintf(path_gen, sizeof path_gen, "%s/gen.txt", dir);
    snprintf(path_ptr, sizeof path_ptr, "%s/ptr.txt", dir);
    snprintf(path_cmd, sizeof path_cmd, "%s/cmd.txt", dir);
    snprintf(path_keys, sizeof path_keys, "%s/keys.txt", dir);
    snprintf(path_go, sizeof path_go, "%s/go.txt", dir);
    snprintf(path_prompt, sizeof path_prompt, "%s/prompt.txt", dir);
    snprintf(path_reply, sizeof path_reply, "%s/reply.txt", dir);
    snprintf(path_model, sizeof path_model, "%s/model.txt", dir);
}

static void write_reply(const char *s) {
    FILE *f = fopen(path_reply, "w");
    if (!f) return;
    fputs(s, f);
    if (s[0] && s[strlen(s) - 1] != '\n') fputc('\n', f);
    fclose(f);
}

/* Abstract SEQPACKET @halcode/Name. Returns 1 if a listener accepts. */
static int abstract_up(const char *name) {
    int fd;
    struct sockaddr_un a;
    size_t n;
    int rc;

    if (!name || name[0] != '@') return 0;
    /* AILang Socket.Create(1,1) is AF_UNIX + SOCK_STREAM, not SEQPACKET. */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    n = strlen(name + 1);
    if (n > sizeof(a.sun_path) - 2) {
        close(fd);
        return 0;
    }
    a.sun_path[0] = '\0';
    memcpy(a.sun_path + 1, name + 1, n);
    rc = connect(fd, (struct sockaddr *)&a,
                 (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n));
    close(fd);
    return rc == 0;
}

/* Busy only for another --agent turn. Leftover --mcp is this Grok session. */
static int other_agent_running(void) {
    DIR *d;
    struct dirent *e;
    char path[64], buf[4096];
    int fd, n, i, busy = 0;
    pid_t self = getpid();

    d = opendir("/proc");
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        pid_t p;
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        p = (pid_t)atoi(e->d_name);
        if (p == self || p == g_agent || p == g_host) continue;
        snprintf(path, sizeof path, "/proc/%d/cmdline", (int)p);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        n = (int)read(fd, buf, (int)sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        i = 0;
        while (i < n) {
            if (strcmp(buf + i, "--agent") == 0) {
                busy = 1;
                break;
            }
            i += (int)strlen(buf + i) + 1;
        }
        if (busy) break;
    }
    closedir(d);
    return busy;
}

static int host_ready(void) {
    return access("/tmp/halcode_app/ready.txt", F_OK) == 0;
}

static void ensure_host(void) {
    char cmd[512];
    int st;
    time_t now = time(NULL);

    if (g_quitting)
        return;
    if (g_host > 0) {
        if (waitpid(g_host, &st, WNOHANG) == g_host) {
            dbg("host pid %d exited", (int)g_host);
            g_host = -1;
            unlink("/tmp/halcode_app/ready.txt");
        }
    }
    if (g_host > 0 && host_ready())
        return;
    if (g_host > 0)
        return;
    if (g_host_t0 != 0 && now - g_host_t0 < 1)
        return;
    g_host_t0 = now;
    dbg("fork host");
    g_host = fork();
    if (g_host == 0) {
        if (chdir(g_app_root) != 0) _exit(127);
        snprintf(cmd, sizeof cmd,
            "exec ./HalCode9000.x --host >>/tmp/halcode_app/host.log 2>&1");
        execl("/bin/bash", "bash", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    dbg("host pid %d", (int)g_host);
}

static void agent_tick(void) {
    char spec[160];
    char cmd[2048];
    int st;
    pid_t w;
    FILE *mf;
    char *nl;
    off_t rsz = 0;
    struct stat sb;

    ensure_host();
    if (access(path_go, F_OK) == 0) {
        if (g_agent > 0 || other_agent_running()) {
            unlink(path_go);
            write_reply("hal: busy — agent turn already in flight");
            if (g_status)
                gtk_label_set_text(GTK_LABEL(g_status), "hal: busy");
            return;
        }
        if (!host_ready()) {
            if (g_go_wait == 0) {
                g_go_wait = time(NULL);
                dbg("go.txt wait for ready.txt");
            }
            if (time(NULL) - g_go_wait < 2) {
                if (g_status)
                    gtk_label_set_text(GTK_LABEL(g_status), "hal: attaching tools");
                return;
            }
            dbg("go.txt ready timeout — starting agent anyway");
        }
        g_go_wait = 0;
        dbg("start agent go.txt");
        unlink(path_go);
        spec[0] = 0;
        mf = fopen(path_model, "r");
        if (mf) {
            if (fgets(spec, (int)sizeof spec, mf) == NULL) spec[0] = 0;
            fclose(mf);
            nl = strchr(spec, '\n');
            if (nl) *nl = 0;
        }
        if (!spec[0]) snprintf(spec, sizeof spec, "deepseek");
        g_agent = fork();
        if (g_agent == 0) {
            if (chdir(g_app_root) != 0) _exit(127);
            snprintf(cmd, sizeof cmd,
                "k=\"$HOME/.halcode\"; set -a; "
                "[ -f \"$k/keys.env\" ] && . \"$k/keys.env\"; "
                "[ -f \"$k/deepseek_key\" ] && export DEEPSEEK_API_KEY=$(tr -d '\\n' < \"$k/deepseek_key\"); "
                "[ -f \"$k/anthropic_key\" ] && export ANTHROPIC_API_KEY=$(tr -d '\\n' < \"$k/anthropic_key\"); "
                "[ -f \"$k/xai_key\" ] && export XAI_API_KEY=$(tr -d '\\n' < \"$k/xai_key\"); "
                "[ -f \"$k/gemini_key\" ] && export GEMINI_API_KEY=$(tr -d '\\n' < \"$k/gemini_key\"); "
                "[ -f \"$k/google_key\" ] && export GOOGLE_API_KEY=$(tr -d '\\n' < \"$k/google_key\"); "
                "set +a; exec ./HalCode9000.x --agent \"%s\" "
                "<%s >/tmp/halcode_app/agent.out 2>/tmp/halcode_app/agent.err",
                spec, path_prompt);
            execl("/bin/bash", "bash", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        if (g_agent < 0) {
            write_reply("hal: busy — could not start agent");
            return;
        }
        g_agent_t0 = time(NULL);
        g_agent_prog = g_agent_t0;
        g_agent_term = 0;
        g_prog_n = -1;
        if (g_status)
            gtk_label_set_text(GTK_LABEL(g_status), "hal: calling model...");
    }
    if (g_agent > 0) {
        time_t now = time(NULL);
        off_t n = 0;
        if (stat("/tmp/halcode_app/agent.err", &sb) == 0) n += sb.st_size;
        if (stat("/tmp/halcode_app/think.txt", &sb) == 0) n += sb.st_size;
        if (stat("/tmp/halcode_app/think.seen", &sb) == 0) n += sb.st_size;
        if (stat(path_reply, &sb) == 0) n += sb.st_size;
        if (n != g_prog_n) {
            g_prog_n = n;
            g_agent_prog = now;
        }
        if (g_agent_term > 0) {
            w = waitpid(g_agent, &st, WNOHANG);
            if (w == g_agent || now - g_agent_term >= 2) {
                if (w != g_agent) {
                    dbg("timeout SIGKILL pid=%d", (int)g_agent);
                    kill(g_agent, SIGKILL);
                    waitpid(g_agent, &st, WNOHANG);
                }
                g_agent = -1;
                g_agent_term = 0;
            }
            return;
        }
        /* stall: no COT/reply/err growth. wall: runaway tool loop cap. */
        if (now - g_agent_prog > 180 || now - g_agent_t0 > 900) {
            dbg("timeout stall=%ld wall=%ld pid=%d prog=%ld",
                (long)(now - g_agent_prog), (long)(now - g_agent_t0),
                (int)g_agent, (long)g_prog_n);
            kill(g_agent, SIGTERM);
            g_agent_term = now;
            unlink("/tmp/halcode_app/wait.txt");
            if (stat(path_reply, &sb) != 0 || sb.st_size <= 0)
                write_reply("hal: agent timed out (no progress 180s)");
            {
                FILE *tf = fopen("/tmp/halcode_app/think.txt", "w");
                if (tf) {
                    fprintf(tf, "[gui] agent timed out stall=%ld wall=%ld\n",
                        (long)(now - g_agent_prog), (long)(now - g_agent_t0));
                    fclose(tf);
                }
            }
            if (g_status)
                gtk_label_set_text(GTK_LABEL(g_status), "hal: timeout");
            return;
        }
        w = waitpid(g_agent, &st, WNOHANG);
        if (w == g_agent) {
            g_agent = -1;
            if (stat(path_reply, &sb) == 0) rsz = sb.st_size;
            dbg("agent exit status=%d reply_bytes=%ld", st, (long)rsz);
            unlink("/tmp/halcode_app/wait.txt");
            if (g_status)
                gtk_label_set_text(GTK_LABEL(g_status),
                    rsz > 0 ? "hal: reply ready" : "hal: empty reply");
        }
    }
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
    if (!g_da) return;
    gtk_widget_get_allocation(g_da, &a);
    if (a.width < 1 || a.height < 1) return;
    {
        int fw = (g_fw >= 16) ? g_fw : 1100;
        int fh = (g_fh >= 16) ? g_fh : 720;
        fx = (int)(wx * (double)fw / (double)a.width);
        fy = (int)(wy * (double)fh / (double)a.height);
        if (fx < 0) fx = 0;
        if (fy < 0) fy = 0;
        if (fx >= fw) fx = fw - 1;
        if (fy >= fh) fy = fh - 1;
    }
    snprintf(buf, sizeof buf, "%s %d %d\n", kind, fx, fy);
    {
        struct stat st;
        FILE *pf;
        if (kind[0] == 'm' && stat(path_ptr, &st) == 0 && st.st_size > 400)
            return;
        pf = fopen(path_ptr, "a");
        if (!pf) return;
        fputs(buf, pf);
        fclose(pf);
    }
    if (kind[0] == 'd')
        dbg("ptr down %d %d", fx, fy);
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
    if (g_quitting)
        return FALSE;
    if (g_prov > 0) {
        int st;
        if (waitpid(g_prov, &st, WNOHANG) == g_prov) {
            dbg("providers update exit %d", st);
            g_prov = -1;
        }
    } else if (access("/tmp/halcode_app/provupd.txt", F_OK) == 0) {
        unlink("/tmp/halcode_app/provupd.txt");
        dbg("fetch_providers.py");
        g_prov = fork();
        if (g_prov == 0) {
            execl("/usr/bin/python3", "python3",
                path_fetch_providers,
                (char *)NULL);
            _exit(127);
        }
        if (g_prov < 0)
            g_prov = -1;
    }
    {
        FILE *cf;
        char *cbuf;
        long sz;
        GtkClipboard *cb;
        cf = fopen("/tmp/halcode_app/copy.txt", "rb");
        if (cf) {
            fseek(cf, 0, SEEK_END);
            sz = ftell(cf);
            fseek(cf, 0, SEEK_SET);
            cbuf = NULL;
            if (sz > 0 && sz < 1024L * 1024L)
                cbuf = malloc((size_t)sz + 1);
            if (cbuf && fread(cbuf, 1, (size_t)sz, cf) == (size_t)sz) {
                cbuf[sz] = 0;
                cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
                gtk_clipboard_set_text(cb, cbuf, (int)sz);
                dbg("copy %ld", sz);
            }
            free(cbuf);
            fclose(cf);
            unlink("/tmp/halcode_app/copy.txt");
        }
    }
    ensure_host();
    g_tickn++;
    if (g_tickn <= 5 || (g_tickn % 60) == 0) {
        struct stat ts, rs;
        long th = (stat("/tmp/halcode_app/think.txt", &ts) == 0) ? (long)ts.st_size : -1;
        long rp = (stat(path_reply, &rs) == 0) ? (long)rs.st_size : -1;
        dbg("tick %d host=%d ready=%d agent=%d think=%ld reply=%ld wait=%d",
            g_tickn, (int)g_host, host_ready(), (int)g_agent, th, rp,
            access("/tmp/halcode_app/wait.txt", F_OK) == 0);
    }
    if (g_status && g_agent < 0 && host_ready())
        gtk_label_set_text(GTK_LABEL(g_status), "hal: ready");
    agent_tick();
    int g = read_gen();
    if (g != g_last_gen && g >= 0) {
        struct timespec ts;
        long now;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        if (g_frame_ms != 0 && now - g_frame_ms < 50)
            return TRUE;
        if (load_frame() == 0) {
            g_last_gen = g;
            g_frame_ms = now;
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
    FILE *pf;
    pid_t dp;
    (void)w;
    (void)data;
    if (g_quitting)
        return;
    g_quitting = 1;
    write_cmd("quit");
    if (g_host > 0) {
        kill(g_host, SIGTERM);
        waitpid(g_host, NULL, 0);
        g_host = -1;
    }
    pf = fopen("/tmp/halcode_desk.pid", "r");
    if (pf) {
        if (fscanf(pf, "%d", &dp) == 1 && dp > 1)
            kill(dp, SIGTERM);
        fclose(pf);
    }
    unlink("/tmp/halcode_app/ready.txt");
    unlink("/tmp/halcode_gtk.pid");
    gtk_main_quit();
}

static void on_menu_cmd(GtkWidget *w, gpointer data) {
    (void)w;
    write_cmd((const char *)data);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, const char *cmd) {
    GtkWidget *it = gtk_menu_item_new_with_label(label);
    g_signal_connect(it, "activate", G_CALLBACK(on_menu_cmd), (gpointer)cmd);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return it;
}

static GtkWidget *build_menubar(void) {
    GtkWidget *mbar = gtk_menu_bar_new();
    GtkWidget *top, *sub;

    top = gtk_menu_item_new_with_label("File");
    sub = gtk_menu_new();
    menu_item(sub, "New conversation", "N");
    menu_item(sub, "Clear chat", "N");
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
    GtkWidget *quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), quit);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(mbar), top);

    top = gtk_menu_item_new_with_label("Session");
    sub = gtk_menu_new();
    menu_item(sub, "Load / resume…", "S");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(mbar), top);

    top = gtk_menu_item_new_with_label("Model");
    sub = gtk_menu_new();
    menu_item(sub, "Pick model…", "M");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(mbar), top);

    top = gtk_menu_item_new_with_label("Config");
    sub = gtk_menu_new();
    menu_item(sub, "Settings…", "C");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(mbar), top);

    top = gtk_menu_item_new_with_label("Help");
    sub = gtk_menu_new();
    menu_item(sub, "TUI command map", "H");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(mbar), top);

    return mbar;
}

static void do_paste(void) {
    GtkClipboard *cb;
    gchar *txt;
    FILE *f;

    cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    txt = gtk_clipboard_wait_for_text(cb);
    if (!txt)
        return;
    f = fopen("/tmp/halcode_app/paste.txt", "w");
    if (f) {
        fputs(txt, f);
        fclose(f);
    }
    dbg("paste %d", (int)strlen(txt));
    g_free(txt);
}

static void on_ctx_copy(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    write_line(path_keys, "y");
}

static void on_ctx_paste(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    do_paste();
}

static GtkWidget *build_ctxmenu(void) {
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *it;

    it = gtk_menu_item_new_with_label("Copy");
    g_signal_connect(it, "activate", G_CALLBACK(on_ctx_copy), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    it = gtk_menu_item_new_with_label("Paste");
    g_signal_connect(it, "activate", G_CALLBACK(on_ctx_paste), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    gtk_widget_show_all(menu);
    if (g_da)
        gtk_menu_attach_to_widget(GTK_MENU(menu), g_da, NULL);
    return menu;
}

static void show_ctxmenu(GdkEventButton *e) {
    if (!g_ctxmenu)
        g_ctxmenu = build_ctxmenu();
    gtk_menu_popup_at_pointer(GTK_MENU(g_ctxmenu), (GdkEvent *)e);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *e, gpointer data) {
    (void)data;
    gtk_widget_grab_focus(w);
    if (e->button == 3) {
        if (e->type == GDK_BUTTON_PRESS)
            show_ctxmenu(e);
        return TRUE;
    }
    if (e->button != 1)
        return TRUE;
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

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data) {
    int dir = 0;
    (void)w;
    (void)data;
    if (e->direction == GDK_SCROLL_UP)
        dir = -1;
    else if (e->direction == GDK_SCROLL_DOWN)
        dir = 1;
    else if (e->direction == GDK_SCROLL_SMOOTH) {
        if (e->delta_y < -0.1)
            dir = -1;
        else if (e->delta_y > 0.1)
            dir = 1;
    }
    if (dir < 0)
        write_ptr("W", e->x, e->y);
    else if (dir > 0)
        write_ptr("w", e->x, e->y);
    return dir != 0;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer data) {
    char buf[32];
    guint32 u;
    (void)w;
    (void)data;
    if ((e->state & GDK_CONTROL_MASK) &&
        (e->keyval == GDK_KEY_q || e->keyval == GDK_KEY_Q)) {
        on_quit(NULL, NULL);
        return TRUE;
    }
    if ((e->state & GDK_CONTROL_MASK) &&
        (e->keyval == GDK_KEY_v || e->keyval == GDK_KEY_V)) {
        do_paste();
        return TRUE;
    }
    if ((e->state & GDK_CONTROL_MASK) &&
        (e->keyval == GDK_KEY_c || e->keyval == GDK_KEY_C)) {
        write_line(path_keys, "y");
        return TRUE;
    }
    if ((e->state & GDK_CONTROL_MASK) &&
        (e->keyval == GDK_KEY_a || e->keyval == GDK_KEY_A)) {
        write_line(path_keys, "a");
        return TRUE;
    }
    if (e->state & GDK_SHIFT_MASK) {
        if (e->keyval == GDK_KEY_Left || e->keyval == GDK_KEY_KP_Left) {
            write_line(path_keys, "l");
            return TRUE;
        }
        if (e->keyval == GDK_KEY_Right || e->keyval == GDK_KEY_KP_Right) {
            write_line(path_keys, "m");
            return TRUE;
        }
    }
    if (e->keyval == GDK_KEY_Return || e->keyval == GDK_KEY_KP_Enter) {
        dbg("key Return");
        write_line(path_keys, "r");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_BackSpace) {
        write_line(path_keys, "b");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Delete || e->keyval == GDK_KEY_KP_Delete) {
        write_line(path_keys, "X");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Left || e->keyval == GDK_KEY_KP_Left) {
        write_line(path_keys, "L");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Right || e->keyval == GDK_KEY_KP_Right) {
        write_line(path_keys, "R");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Up || e->keyval == GDK_KEY_KP_Up) {
        write_line(path_keys, "U");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Down || e->keyval == GDK_KEY_KP_Down) {
        write_line(path_keys, "D");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Home || e->keyval == GDK_KEY_KP_Home) {
        write_line(path_keys, "H");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_End || e->keyval == GDK_KEY_KP_End) {
        write_line(path_keys, "E");
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Tab || e->keyval == GDK_KEY_KP_Tab ||
        e->keyval == GDK_KEY_ISO_Left_Tab) {
        write_line(path_keys, "T");
        return TRUE;
    }
    u = gdk_keyval_to_unicode(e->keyval);
    if (u >= 32 && u < 127) {
        snprintf(buf, sizeof buf, "c %u", (unsigned)u);
        write_line(path_keys, buf);
        return TRUE;
    }
    /* keypad when unicode is empty: still emit ASCII */
    switch (e->keyval) {
    case GDK_KEY_KP_0: write_line(path_keys, "c 48"); return TRUE;
    case GDK_KEY_KP_1: write_line(path_keys, "c 49"); return TRUE;
    case GDK_KEY_KP_2: write_line(path_keys, "c 50"); return TRUE;
    case GDK_KEY_KP_3: write_line(path_keys, "c 51"); return TRUE;
    case GDK_KEY_KP_4: write_line(path_keys, "c 52"); return TRUE;
    case GDK_KEY_KP_5: write_line(path_keys, "c 53"); return TRUE;
    case GDK_KEY_KP_6: write_line(path_keys, "c 54"); return TRUE;
    case GDK_KEY_KP_7: write_line(path_keys, "c 55"); return TRUE;
    case GDK_KEY_KP_8: write_line(path_keys, "c 56"); return TRUE;
    case GDK_KEY_KP_9: write_line(path_keys, "c 57"); return TRUE;
    case GDK_KEY_KP_Decimal: write_line(path_keys, "c 46"); return TRUE;
    case GDK_KEY_KP_Add: write_line(path_keys, "c 43"); return TRUE;
    case GDK_KEY_KP_Subtract: write_line(path_keys, "c 45"); return TRUE;
    case GDK_KEY_KP_Multiply: write_line(path_keys, "c 42"); return TRUE;
    case GDK_KEY_KP_Divide: write_line(path_keys, "c 47"); return TRUE;
    case GDK_KEY_KP_Equal: write_line(path_keys, "c 61"); return TRUE;
    default: break;
    }
    return FALSE;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : getenv("HALCODE_APP_STATE");
    if (!dir || !dir[0]) dir = "/tmp/halcode_app";
    resolve_app_root();
    snprintf(path_fetch_providers, sizeof path_fetch_providers,
        "%s/scripts/fetch_providers.py", g_app_root);
    paths_init(dir);
    mkdir(dir, 0755);
    unlink("/tmp/halcode_app/ready.txt");
    unlink("/tmp/halcode_app/cmd.txt");
    unlink("/tmp/halcode_app/wait.txt");
    unlink("/tmp/halcode_app/go.txt");
    unlink("/tmp/halcode_app/prompt.txt");
    unlink("/tmp/halcode_app/reply.txt");
    unlink("/tmp/halcode_app/reply.seen");
    unlink("/tmp/halcode_app/think.txt");
    unlink("/tmp/halcode_app/think.seen");
    unlink("/tmp/halcode_app/copy.txt");
    unlink("/tmp/halcode_app/paste.txt");
    {
        FILE *lf = fopen("/tmp/halcode_gtk.pid", "r");
        pid_t old = 0;
        if (lf) {
            if (fscanf(lf, "%d", &old) == 1) { /* ok */ }
            fclose(lf);
            if (old > 1 && old != getpid() && kill(old, 0) == 0) {
                fprintf(stderr, "halcode_shell_gtk already running (pid %d)\n", (int)old);
                return 1;
            }
        }
        lf = fopen("/tmp/halcode_gtk.pid", "w");
        if (lf) {
            fprintf(lf, "%d\n", (int)getpid());
            fclose(lf);
        }
    }
    dbg("main dir=%s", dir);

    gtk_init(&argc, &argv);

    g_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_win), "HalCode9000");
    {
        int ww, wh;
        default_window_size(&ww, &wh);
        gtk_window_set_default_size(GTK_WINDOW(g_win), ww, wh);
    }
    g_signal_connect(g_win, "destroy", G_CALLBACK(on_quit), NULL);
    g_signal_connect(g_win, "key-press-event", G_CALLBACK(on_key), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g_win), root);

    gtk_box_pack_start(GTK_BOX(root), build_menubar(), FALSE, FALSE, 0);

    g_da = gtk_drawing_area_new();
    gtk_widget_set_hexpand(g_da, TRUE);
    gtk_widget_set_vexpand(g_da, TRUE);
    gtk_widget_set_size_request(g_da, 640, 400);
    gtk_widget_set_can_focus(g_da, TRUE);
    gtk_widget_add_events(g_da,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_BUTTON1_MOTION_MASK |
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
        GDK_KEY_PRESS_MASK | GDK_STRUCTURE_MASK);
    g_signal_connect(g_da, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(g_da, "configure-event", G_CALLBACK(on_da_configure), NULL);
    g_signal_connect(g_da, "button-press-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(g_da, "button-release-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(g_da, "motion-notify-event", G_CALLBACK(on_motion), NULL);
    g_signal_connect(g_da, "scroll-event", G_CALLBACK(on_scroll), NULL);
    gtk_box_pack_start(GTK_BOX(root), g_da, TRUE, TRUE, 0);

    g_status = gtk_label_new("waiting for kernel");
    gtk_widget_set_name(g_status, "statbar");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0.0);
    gtk_box_pack_start(GTK_BOX(root), g_status, FALSE, FALSE, 0);

    g_timeout_add(16, poll_tick, NULL);
    gtk_widget_show_all(g_win);
    gtk_widget_grab_focus(g_da);
    gtk_main();

    write_cmd("quit");
    if (g_host > 0) {
        kill(g_host, SIGTERM);
        waitpid(g_host, NULL, 0);
        g_host = -1;
    }
    if (g_surf) cairo_surface_destroy(g_surf);
    return 0;
}
