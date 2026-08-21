/*
 * browser-mini - minimal WebKitGTK 6.0 (GTK4) page viewer
 *
 * build (Debian/derivatives: apt install build-essential pkg-config
 *        libgtk-4-dev libwebkitgtk-6.0-dev libsoup-3.0-dev):
 *
 *   gcc -O2 -Wall -Wextra -o browser-mini browser-mini.c \
 *       $(pkg-config --cflags --libs gtk4 webkitgtk-6.0 libsoup-3.0)
 *
 * Run with -h for the full option and key list.
 */

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_TITLE    "Minibrowser"
#define DEFAULT_APP_ID   "browser-mini"
#define DEFAULT_CLIP_CMD "wl-copy"

/* Directory name used under ~/.local/share and ~/.cache for profiles.
 * Kept as "wkview" on purpose: renaming it would orphan existing
 * cookie jars / logins. Move those dirs by hand if you change it. */
#define PROFILE_DIR_NAME "wkview"

#define ZOOM_STEP 1.1
#define ZOOM_MIN  0.25
#define ZOOM_MAX  5.0

#define URL_TOAST_SECONDS 4

/* Modifiers we consider "significant" when matching a shortcut.
 * Shift is deliberately absent: it is part of the key, not the combo. */
#define MOD_MASK_ALL (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)

/* -------------------------------------------------------------- globals */

static WebKitUserContentManager *g_ucm;
static WebKitSettings           *g_settings;
static WebKitNetworkSession     *g_session;

static const char *g_css_path;
static gboolean    g_deny_media;
static gboolean    g_quiet;
static int         g_windows;

static char       *g_download_dir;
static char       *g_profile;              /* NULL -> "default" */
static gboolean    g_private;
static gboolean    g_clear_data;
static char       *g_user_agent;

static char       *g_title;                /* window title, see --title    */
static char       *g_app_id;               /* wayland app_id / X11 WM_CLASS */
static gboolean    g_follow_page_title = TRUE; /* see --title / --page-title */
static char       *g_clip_cmd;             /* --clip-cmd, NULL -> wl-copy  */
static double      g_zoom = 1.0;

static GdkModifierType g_mod      = GDK_CONTROL_MASK;
static const char     *g_mod_name = "Ctrl";

#define LOG(...) G_STMT_START { if (!g_quiet) g_printerr (__VA_ARGS__); } G_STMT_END

/* Per-window state. Owned by the GtkWindow (see window_new). */
typedef struct {
    GtkWidget     *win;
    WebKitWebView *view;
    GtkWidget     *urlbar;      /* GtkEntry, top left, hidden by default  */
    GtkWidget     *toast;       /* GtkLabel, top right, hidden by default */
    guint          toast_id;
    gboolean       primary;
} Win;

/* ---------------------------------------------------------- prototypes */

static GtkWidget *window_new (WebKitWebView *view, gboolean primary);
static gboolean   on_key     (GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType state, gpointer user_data);
static void       on_close   (GtkWindow *w, gpointer user_data);
static gboolean   on_decide_policy (WebKitWebView *view, WebKitPolicyDecision *decision,
                                    WebKitPolicyDecisionType type, gpointer u);
static gboolean   on_permission    (WebKitWebView *view, WebKitPermissionRequest *req,
                                    gpointer u);
static GtkWidget *on_create        (WebKitWebView *view, WebKitNavigationAction *act,
                                    gpointer u);
static void       on_ready_to_show (WebKitWebView *view, gpointer u);
static void       on_session_download_started (WebKitNetworkSession *session,
                                               WebKitDownload *download, gpointer u);

/* ------------------------------------------------------------- helpers */

static void
settings_set_bool_if_exists (WebKitSettings *s, const char *prop, gboolean value)
{
    if (g_object_class_find_property (G_OBJECT_GET_CLASS (s), prop))
        g_object_set (G_OBJECT (s), prop, value, NULL);
    else
        LOG ("note: WebKitSettings property not supported: %s\n", prop);
}

static void
object_set_string_if_exists (GObject *o, const char *prop, const char *value)
{
    if (g_object_class_find_property (G_OBJECT_GET_CLASS (o), prop))
        g_object_set (o, prop, value, NULL);
}

/* "example.com" -> "https://example.com", "./page.html" -> "file:///...". */
static char *
normalize_uri (const char *in)
{
    if (!in)
        return NULL;

    char *s = g_strstrip (g_strdup (in));
    if (!*s) {
        g_free (s);
        return NULL;
    }

    if (strstr (s, "://")      ||
        g_str_has_prefix (s, "about:")  ||
        g_str_has_prefix (s, "data:")   ||
        g_str_has_prefix (s, "mailto:"))
        return s;

    if (g_file_test (s, G_FILE_TEST_EXISTS)) {
        char *abs = g_canonicalize_filename (s, NULL);
        char *uri = g_filename_to_uri (abs, NULL, NULL);
        g_free (abs);
        if (uri) {
            g_free (s);
            return uri;
        }
    }

    char *https = g_strconcat ("https://", s, NULL);
    g_free (s);
    return https;
}

static gboolean
parse_mod (const char *name)
{
    if (!g_ascii_strcasecmp (name, "ctrl") || !g_ascii_strcasecmp (name, "control")) {
        g_mod = GDK_CONTROL_MASK; g_mod_name = "Ctrl";  return TRUE;
    }
    if (!g_ascii_strcasecmp (name, "alt")  || !g_ascii_strcasecmp (name, "mod1")) {
        g_mod = GDK_ALT_MASK;     g_mod_name = "Alt";   return TRUE;
    }
    if (!g_ascii_strcasecmp (name, "super") || !g_ascii_strcasecmp (name, "win")) {
        g_mod = GDK_SUPER_MASK;   g_mod_name = "Super"; return TRUE;
    }
    if (!g_ascii_strcasecmp (name, "meta")) {
        g_mod = GDK_META_MASK;    g_mod_name = "Meta";  return TRUE;
    }
    return FALSE;
}

/* --------------------------------------------------------------- usage */

static void
usage (const char *argv0, gboolean to_stdout)
{
    char *text = g_strdup_printf (
"%s - minimal WebKitGTK 6.0 / GTK4 page viewer\n"
"\n"
"usage: %s URL [options]\n"
"\n"
"window:\n"
"  --title NAME        pin the window title to NAME (default: follow the\n"
"                      page <title>, falling back to \"%s\")\n"
"  --page-title        keep following the page <title> even with --title,\n"
"                      which then only supplies the fallback\n"
"  --app-id ID         Wayland app_id / X11 WM_CLASS (default: \"%s\")\n"
"                      this is what sway matches in for_window [app_id=...]\n"
"  --zoom N            initial zoom level (default: 1.0)\n"
"\n"
"keys:\n"
"  --mod KEY           modifier for the shortcuts below:\n"
"                      ctrl | alt | super | meta (default: ctrl)\n"
"  --clip-cmd CMD      command used by %s+Y, gets the URL on stdin\n"
"                      (default: \"%s\"; \"internal\" uses the GTK clipboard)\n"
"\n"
"page:\n"
"  --css FILE          inject FILE as a user stylesheet\n"
"  --devtools          open the inspector once the first page commits\n"
"  --no-media          deny camera / microphone / screen-share requests\n"
"\n"
"session:\n"
"  --download-dir DIR  download target (default: XDG download dir)\n"
"  --profile NAME      named profile, persists cookies (default: \"default\")\n"
"  --clear-data        wipe the profile's data and cache before starting\n"
"  --private           ephemeral session, ignores --profile/--clear-data\n"
"  --user-agent UA     set an explicit user agent string\n"
"  --ua-chrome | --ua-win-chrome | --ua-win-edge | --ua-firefox | --ua-safari\n"
"\n"
"misc:\n"
"  -q, --quiet         silence the diagnostic output on stderr\n"
"  -h, --help          this text\n"
"\n"
"key bindings (<mod> = %s):\n"
"  <mod>+R  or F5      reload the page\n"
"  <mod>+Shift+R       re-read the --css file, then reload\n"
"  <mod>+D  or F12     toggle the developer tools\n"
"  <mod>+Shift+I       toggle the developer tools\n"
"  <mod>+O             open the URL bar (top left, white on black)\n"
"                      Enter loads, Esc cancels\n"
"  <mod>+P             show the current URL (top right) for %d seconds\n"
"  <mod>+plus          zoom in\n"
"  <mod>+minus         zoom out\n"
"  <mod>+0             reset zoom to 100%%\n"
"  <mod>+Y             copy the current URL using --clip-cmd\n"
"\n"
"  Only the combinations listed above are intercepted; everything else\n"
"  (<mod>+A, <mod>+C, <mod>+V, ...) goes straight to the page, so select-all\n"
"  and copy/paste keep working inside input fields. While the URL bar is\n"
"  open every key except Esc belongs to it, so <mod>+A selects its text.\n"
"%s",
        DEFAULT_TITLE, argv0, DEFAULT_TITLE, DEFAULT_APP_ID, g_mod_name,
        DEFAULT_CLIP_CMD, g_mod_name, URL_TOAST_SECONDS,
        g_mod == GDK_CONTROL_MASK
            ? "  Use --mod alt if you would rather keep Ctrl+P for the page's"
              " print dialog.\n"
            : "");

    if (to_stdout)
        g_print ("%s", text);
    else
        g_printerr ("%s", text);

    g_free (text);
}

static void
usage_short (const char *argv0)
{
    g_printerr ("usage: %s URL [options]   (try %s -h)\n", argv0, argv0);
}

/* ----------------------------------------------------------------- CSS */

/* User stylesheet injected into the pages (--css). */
static void
css_reload (void)
{
    gchar  *data = NULL;
    GError *err  = NULL;

    webkit_user_content_manager_remove_all_style_sheets (g_ucm);
    if (!g_css_path)
        return;

    if (!g_file_get_contents (g_css_path, &data, NULL, &err)) {
        g_printerr ("css: %s\n", err->message);
        g_error_free (err);
        return;
    }

    WebKitUserStyleSheet *ss =
        webkit_user_style_sheet_new (data,
                                     WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                     WEBKIT_USER_STYLE_LEVEL_USER,
                                     NULL, NULL);
    webkit_user_content_manager_add_style_sheet (g_ucm, ss);
    webkit_user_style_sheet_unref (ss);
    g_free (data);
    LOG ("css: injected %s\n", g_css_path);
}

/* Styling for our own widgets (URL bar + toast), not for the page. */
static void
ui_css_install (void)
{
    static const char css[] =
        "entry.mini-urlbar, entry.mini-urlbar > text {"
        "  background-color: #000; background-image: none; color: #fff;"
        "  caret-color: #fff; border: none; border-radius: 0;"
        "  box-shadow: none; outline: none;"
        "  font-family: monospace; font-size: 12pt;"
        "  padding: 6px 10px; margin: 0;"
        "}"
        /* grey, not white, so a select-all does not turn the bar into a
           white block - it stays white text on black */
        "entry.mini-urlbar > text > selection {"
        "  background-color: #444; color: #fff;"
        "}"
        "label.mini-toast {"
        "  background-color: #000; color: #fff;"
        "  font-family: monospace; font-size: 11pt;"
        "  padding: 6px 10px;"
        "}";

    GtkCssProvider *p = gtk_css_provider_new ();
#if GTK_CHECK_VERSION (4, 12, 0)
    gtk_css_provider_load_from_string (p, css);
#else
    gtk_css_provider_load_from_data (p, css, -1);
#endif
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (p),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (p);
}

/* ------------------------------------------------------- rm -rf helper */

static gboolean
rm_rf (const char *path)
{
    if (!path || !*path || !g_file_test (path, G_FILE_TEST_EXISTS))
        return TRUE;

    if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
        GError *err = NULL;
        GDir   *d   = g_dir_open (path, 0, &err);
        if (!d) {
            g_printerr ("clear-data: cannot open dir %s: %s\n", path, err->message);
            g_error_free (err);
            return FALSE;
        }

        const char *name;
        while ((name = g_dir_read_name (d))) {
            char *child = g_build_filename (path, name, NULL);
            gboolean ok = rm_rf (child);
            g_free (child);
            if (!ok) {
                g_dir_close (d);
                return FALSE;
            }
        }
        g_dir_close (d);

        if (g_rmdir (path) != 0) {
            g_printerr ("clear-data: cannot remove dir %s\n", path);
            return FALSE;
        }
        return TRUE;
    }

    if (g_remove (path) != 0) {
        g_printerr ("clear-data: cannot remove file %s\n", path);
        return FALSE;
    }
    return TRUE;
}

static void
profile_dirs (const char *profile, char **out_data_dir, char **out_cache_dir)
{
    if (!profile || !*profile)
        profile = "default";

    *out_data_dir  = g_build_filename (g_get_user_data_dir (),  PROFILE_DIR_NAME, profile, NULL);
    *out_cache_dir = g_build_filename (g_get_user_cache_dir (), PROFILE_DIR_NAME, profile, NULL);
}

/* ----------------------------------------------------------- clipboard */

static void
clip_reap (GPid pid, int status, gpointer u)
{
    (void) status; (void) u;
    g_spawn_close_pid (pid);
}

static gboolean
clip_spawn (const char *cmd, const char *text)
{
    char  **argv = NULL;
    GError *err  = NULL;

    if (!g_shell_parse_argv (cmd, NULL, &argv, &err)) {
        g_printerr ("clip: bad --clip-cmd %s: %s\n", cmd, err->message);
        g_error_free (err);
        return FALSE;
    }

    GPid pid;
    int  in_fd = -1;
    gboolean ok = g_spawn_async_with_pipes (NULL, argv, NULL,
                                            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                            NULL, NULL, &pid, &in_fd, NULL, NULL, &err);
    g_strfreev (argv);

    if (!ok) {
        g_printerr ("clip: cannot run %s: %s\n", cmd, err->message);
        g_error_free (err);
        return FALSE;
    }

    size_t len = strlen (text), off = 0;
    while (off < len) {
        ssize_t n = write (in_fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_printerr ("clip: write failed: %s\n", g_strerror (errno));
            break;
        }
        off += (size_t) n;
    }
    close (in_fd);
    g_child_watch_add (pid, clip_reap, NULL);
    return TRUE;
}

static void
clipboard_copy (Win *w, const char *text)
{
    if (!text || !*text)
        return;

    const char *cmd = g_clip_cmd ? g_clip_cmd : DEFAULT_CLIP_CMD;

    if (!g_ascii_strcasecmp (cmd, "internal") || !g_ascii_strcasecmp (cmd, "gtk")) {
        gdk_clipboard_set_text (gtk_widget_get_clipboard (w->win), text);
        LOG ("clip: copied via GTK clipboard\n");
        return;
    }

    if (clip_spawn (cmd, text))
        LOG ("clip: copied via %s\n", cmd);
}

/* -------------------------------------------------------------- download */

static char *
unique_download_path (const char *dir, const char *suggested)
{
    char *path = g_build_filename (dir, suggested, NULL);
    if (!g_file_test (path, G_FILE_TEST_EXISTS))
        return path;

    const char *dot = strrchr (suggested, '.');
    char       *stem;
    const char *ext = "";

    if (dot && dot != suggested) {
        stem = g_strndup (suggested, (gsize) (dot - suggested));
        ext  = dot;                       /* includes the '.' */
    } else {
        stem = g_strdup (suggested);
    }

    for (int i = 1; i < 10000; i++) {
        char *name = g_strdup_printf ("%s (%d)%s", stem, i, ext);
        g_free (path);
        path = g_build_filename (dir, name, NULL);
        g_free (name);
        if (!g_file_test (path, G_FILE_TEST_EXISTS))
            break;
    }

    g_free (stem);
    return path;
}

static gboolean
on_decide_destination (WebKitDownload *download,
                       const gchar    *suggested_filename,
                       gpointer        u)
{
    (void) u;

    const char *dir = (g_download_dir && *g_download_dir) ? g_download_dir : ".";

    char *absdir;
    if (g_path_is_absolute (dir)) {
        absdir = g_strdup (dir);
    } else {
        char *cwd = g_get_current_dir ();
        absdir = g_build_filename (cwd, dir, NULL);
        g_free (cwd);
    }

    if (g_mkdir_with_parents (absdir, 0700) != 0) {
        g_printerr ("download: cannot create dir: %s\n", absdir);
        g_free (absdir);
        webkit_download_cancel (download);
        return TRUE;
    }

    if (!suggested_filename || !*suggested_filename)
        suggested_filename = "download";

    /* WebKitGTK 2.52.x wants an absolute filesystem path here. */
    char *path = unique_download_path (absdir, suggested_filename);

    webkit_download_set_destination (download, path);
    LOG ("download: %s -> %s\n", suggested_filename, path);

    g_free (path);
    g_free (absdir);
    return TRUE;                          /* handled */
}

static void
on_download_failed (WebKitDownload *download, GError *err, gpointer u)
{
    (void) u;
    const char *dest = webkit_download_get_destination (download);
    g_printerr ("download: FAILED dest=%s err=%s\n",
                dest ? dest : "(none)", err ? err->message : "(unknown)");
}

static void
on_download_finished (WebKitDownload *download, gpointer u)
{
    (void) u;
    const char *dest = webkit_download_get_destination (download);
    LOG ("download: FINISHED dest=%s\n", dest ? dest : "(none)");
}

/* Log at most once per 10% instead of once per received chunk. */
static void
on_download_received_data (WebKitDownload *download, guint64 data_length, gpointer u)
{
    (void) data_length; (void) u;

    if (g_quiet)
        return;

    int decile = (int) (webkit_download_get_estimated_progress (download) * 10.0);
    int last   = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (download), "decile"));
    if (decile <= last)
        return;

    g_object_set_data (G_OBJECT (download), "decile", GINT_TO_POINTER (decile));
    g_printerr ("download: progress %d%%\n", decile * 10);
}

static void
download_wire (WebKitDownload *download)
{
    g_signal_connect (download, "decide-destination", G_CALLBACK (on_decide_destination), NULL);
    g_signal_connect (download, "received-data",      G_CALLBACK (on_download_received_data), NULL);
    g_signal_connect (download, "finished",           G_CALLBACK (on_download_finished), NULL);
    g_signal_connect (download, "failed",             G_CALLBACK (on_download_failed), NULL);
}

static void
on_session_download_started (WebKitNetworkSession *session,
                             WebKitDownload       *download,
                             gpointer              u)
{
    (void) session; (void) u;
    WebKitURIRequest *req = webkit_download_get_request (download);
    LOG ("download: session started uri=%s\n",
         req ? webkit_uri_request_get_uri (req) : "(unknown)");
    download_wire (download);
}

static gboolean
on_decide_policy (WebKitWebView            *view,
                  WebKitPolicyDecision     *decision,
                  WebKitPolicyDecisionType  type,
                  gpointer                  u)
{
    (void) view; (void) u;

    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
        return FALSE;

    WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION (decision);
    gboolean supported = webkit_response_policy_decision_is_mime_type_supported (r);

    WebKitURIResponse *resp = webkit_response_policy_decision_get_response (r);
    const char *mime = resp ? webkit_uri_response_get_mime_type (resp) : NULL;

    gboolean attachment = FALSE;
    if (resp) {
        SoupMessageHeaders *hdrs = webkit_uri_response_get_http_headers (resp);
        if (hdrs) {
            const char *cd = soup_message_headers_get_one (hdrs, "Content-Disposition");
            if (cd && g_strrstr (cd, "attachment"))
                attachment = TRUE;
        }
    }

    if (!supported || attachment) {
        LOG ("download: policy -> download mime=%s supported=%d attachment=%d\n",
             mime ? mime : "(unknown)", supported, attachment);
        webkit_policy_decision_download (decision);
        return TRUE;                      /* handled */
    }

    return FALSE;                         /* let WebKit continue normally */
}

/* ----------------------------------------------------------- permissions */

static gboolean
media_permission (WebKitPermissionRequest *req)
{
    if (g_deny_media)
        webkit_permission_request_deny (req);
    else
        webkit_permission_request_allow (req);
    return TRUE;
}

static gboolean
on_permission (WebKitWebView *view, WebKitPermissionRequest *req, gpointer u)
{
    (void) view; (void) u;

    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST (req)) {
        WebKitUserMediaPermissionRequest *m = WEBKIT_USER_MEDIA_PERMISSION_REQUEST (req);
        LOG ("perm: media audio=%d video=%d screen=%d -> %s\n",
             webkit_user_media_permission_is_for_audio_device (m),
             webkit_user_media_permission_is_for_video_device (m),
             webkit_user_media_permission_is_for_display_device (m),
             g_deny_media ? "deny" : "allow");
        return media_permission (req);
    }

    if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST (req))
        return media_permission (req);

    if (WEBKIT_IS_POINTER_LOCK_PERMISSION_REQUEST (req)) {
        webkit_permission_request_allow (req);
        return TRUE;
    }

    webkit_permission_request_deny (req);
    return TRUE;
}

/* ------------------------------------------------------------ inspector */

static void
inspector_toggle (WebKitWebView *view)
{
    WebKitWebInspector *insp = webkit_web_view_get_inspector (view);
    if (!insp)
        return;

    if (webkit_web_inspector_get_web_view (insp))
        webkit_web_inspector_close (insp);
    else
        webkit_web_inspector_show (insp);
}

/* open the inspector once, after the first commit (--devtools) */
static void
on_load_changed (WebKitWebView *view, WebKitLoadEvent ev, gpointer u)
{
    (void) u;
    if (ev != WEBKIT_LOAD_COMMITTED)
        return;
    g_signal_handlers_disconnect_by_func (view, G_CALLBACK (on_load_changed), NULL);
    webkit_web_inspector_show (webkit_web_view_get_inspector (view));
}

/* --------------------------------------------------------------- toast */

static gboolean
toast_timeout (gpointer u)
{
    Win *w = u;
    gtk_widget_set_visible (w->toast, FALSE);
    w->toast_id = 0;
    return G_SOURCE_REMOVE;
}

static void
toast_show (Win *w, const char *text, guint seconds)
{
    gtk_label_set_text (GTK_LABEL (w->toast), text);
    gtk_widget_set_visible (w->toast, TRUE);

    if (w->toast_id)
        g_source_remove (w->toast_id);
    w->toast_id = g_timeout_add_seconds (seconds, toast_timeout, w);
}

/* -------------------------------------------------------------- urlbar */

static void
urlbar_hide (Win *w)
{
    gtk_widget_set_visible (w->urlbar, FALSE);
    gtk_widget_grab_focus (GTK_WIDGET (w->view));
}

static void
urlbar_show (Win *w)
{
    const char *uri = webkit_web_view_get_uri (w->view);

    gtk_editable_set_text (GTK_EDITABLE (w->urlbar), uri ? uri : "");
    gtk_widget_set_visible (w->urlbar, TRUE);
    gtk_widget_grab_focus (w->urlbar);
    gtk_editable_select_region (GTK_EDITABLE (w->urlbar), 0, -1);
}

static void
on_urlbar_activate (GtkEntry *entry, gpointer u)
{
    Win  *w   = u;
    char *uri = normalize_uri (gtk_editable_get_text (GTK_EDITABLE (entry)));

    urlbar_hide (w);

    if (uri) {
        LOG ("load: %s\n", uri);
        webkit_web_view_load_uri (w->view, uri);
        g_free (uri);
    }
}

/* ---------------------------------------------------------------- zoom */

static void
zoom_by (Win *w, double factor)
{
    double z = webkit_web_view_get_zoom_level (w->view) * factor;

    z = CLAMP (z, ZOOM_MIN, ZOOM_MAX);
    webkit_web_view_set_zoom_level (w->view, z);

    char *msg = g_strdup_printf ("zoom %.0f%%", z * 100.0);
    toast_show (w, msg, 1);
    g_free (msg);
}

static void
zoom_reset (Win *w)
{
    webkit_web_view_set_zoom_level (w->view, 1.0);
    toast_show (w, "zoom 100%", 1);
}

/* ------------------------------------------------------------- windows */

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint code,
        GdkModifierType state, gpointer user_data)
{
    (void) c; (void) code;

    Win *w = user_data;

    /* While our URL bar is open it owns the keyboard: only Esc is ours.
     * That is what keeps <mod>+A (select all), <mod>+U, Home/End etc.
     * working inside the entry. */
    if (gtk_widget_get_visible (w->urlbar)) {
        if (keyval == GDK_KEY_Escape) {
            urlbar_hide (w);
            return TRUE;
        }
        return FALSE;
    }

    guint    key   = gdk_keyval_to_lower (keyval);
    gboolean mod   = (state & MOD_MASK_ALL) == g_mod;   /* exactly our modifier */
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;

    /* --- unmodified function keys ------------------------------------ */
    if (keyval == GDK_KEY_F12) {
        inspector_toggle (w->view);
        return TRUE;
    }
    if (keyval == GDK_KEY_F5) {
        webkit_web_view_reload (w->view);
        return TRUE;
    }

    if (!mod)
        return FALSE;

    /* --- <mod>+Shift+key --------------------------------------------- */
    if (shift) {
        switch (key) {
        case GDK_KEY_i:
            inspector_toggle (w->view);
            return TRUE;
        case GDK_KEY_r:
            css_reload ();
            webkit_web_view_reload (w->view);
            return TRUE;
        default:
            break;                        /* fall through to zoom keys */
        }
    }

    /* --- <mod>+key ---------------------------------------------------- */
    switch (key) {
    case GDK_KEY_r:
        if (shift)
            break;
        webkit_web_view_reload (w->view);
        return TRUE;

    case GDK_KEY_d:
        inspector_toggle (w->view);
        return TRUE;

    case GDK_KEY_o:
        urlbar_show (w);
        return TRUE;

    case GDK_KEY_p: {
        const char *uri = webkit_web_view_get_uri (w->view);
        toast_show (w, uri ? uri : "(no url)", URL_TOAST_SECONDS);
        return TRUE;
    }

    case GDK_KEY_y:
        clipboard_copy (w, webkit_web_view_get_uri (w->view));
        return TRUE;

    /* '+' usually needs Shift, and some layouts send '=' or the keypad key */
    case GDK_KEY_plus:
    case GDK_KEY_equal:
    case GDK_KEY_KP_Add:
        zoom_by (w, ZOOM_STEP);
        return TRUE;

    case GDK_KEY_minus:
    case GDK_KEY_underscore:
    case GDK_KEY_KP_Subtract:
        zoom_by (w, 1.0 / ZOOM_STEP);
        return TRUE;

    case GDK_KEY_0:
    case GDK_KEY_KP_0:
        zoom_reset (w);
        return TRUE;

    default:
        break;
    }

    return FALSE;                         /* not ours - hand it to the page */
}

static void
on_close (GtkWindow *win, gpointer user_data)
{
    (void) win;
    Win *w = user_data;

    if (w->primary || --g_windows <= 0) {
        GMainLoop *loop = g_object_get_data (G_OBJECT (w->win), "loop");
        if (loop)
            g_main_loop_quit (loop);
    }
}

static void
win_free (gpointer data)
{
    Win *w = data;
    if (w->toast_id)
        g_source_remove (w->toast_id);
    g_free (w);
}

static void
on_title (GObject *obj, GParamSpec *ps, gpointer user_data)
{
    (void) ps;
    Win        *w = user_data;
    const char *t = webkit_web_view_get_title (WEBKIT_WEB_VIEW (obj));
    gtk_window_set_title (GTK_WINDOW (w->win), (t && *t) ? t : g_title);
}

/* --------------------------------------------------------------- app id */

/* Wayland: the xdg_toplevel app_id, i.e. what `swaymsg -t get_tree` shows
 * as app_id and what sway's `for_window [app_id="..."]` rules match on.
 * X11:     the WM_CLASS pair.
 *
 * Both come from g_get_prgname() unless a GtkApplication supplies an id;
 * with prgname unset a Wayland toplevel falls back to the literal "GTK
 * Application". So g_set_prgname() in main() is what actually fixes this.
 *
 * X11 needs nothing further: a toplevel takes WM_CLASS's res_name from
 * prgname and falls back to it for res_class too, so calling
 * gdk_x11_display_set_program_class() would only repeat what prgname
 * already did - and it is deprecated since GTK 4.18. */
static void
on_window_map (GtkWidget *win, gpointer u)
{
    (void) win; (void) u;

#ifdef GDK_WINDOWING_WAYLAND
    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (win));
    if (surface && GDK_IS_WAYLAND_TOPLEVEL (surface))
        gdk_wayland_toplevel_set_application_id (GDK_TOPLEVEL (surface), g_app_id);
#endif
}

static GtkWidget *
window_new (WebKitWebView *view, gboolean primary)
{
    Win *w    = g_new0 (Win, 1);
    w->view    = view;
    w->primary = primary;
    w->win     = gtk_window_new ();

    gtk_window_set_default_size (GTK_WINDOW (w->win), 1280, 800);
    gtk_window_set_title (GTK_WINDOW (w->win), g_title);

    GtkWidget *overlay = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (overlay), GTK_WIDGET (view));

    /* URL bar: top left, white on black, hidden until <mod>+O */
    w->urlbar = gtk_entry_new ();
    gtk_entry_set_has_frame (GTK_ENTRY (w->urlbar), FALSE);
    gtk_widget_add_css_class (w->urlbar, "mini-urlbar");
    gtk_widget_set_halign (w->urlbar, GTK_ALIGN_START);
    gtk_widget_set_valign (w->urlbar, GTK_ALIGN_START);
    gtk_widget_set_size_request (w->urlbar, 720, -1);
    gtk_widget_set_visible (w->urlbar, FALSE);
    g_signal_connect (w->urlbar, "activate", G_CALLBACK (on_urlbar_activate), w);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->urlbar);

    /* Toast: top right, click-through, hidden until <mod>+P or zoom */
    w->toast = gtk_label_new ("");
    gtk_widget_add_css_class (w->toast, "mini-toast");
    gtk_widget_set_halign (w->toast, GTK_ALIGN_END);
    gtk_widget_set_valign (w->toast, GTK_ALIGN_START);
    gtk_widget_set_can_target (w->toast, FALSE);
    gtk_widget_set_visible (w->toast, FALSE);
    gtk_label_set_ellipsize (GTK_LABEL (w->toast), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (w->toast), 90);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->toast);

    gtk_window_set_child (GTK_WINDOW (w->win), overlay);

    GtkEventController *kc = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (kc, GTK_PHASE_CAPTURE);
    g_signal_connect (kc, "key-pressed", G_CALLBACK (on_key), w);
    gtk_widget_add_controller (w->win, kc);

    if (g_follow_page_title)
        g_signal_connect (view, "notify::title", G_CALLBACK (on_title), w);

    g_signal_connect (w->win, "map", G_CALLBACK (on_window_map), NULL);
    g_signal_connect (w->win, "destroy", G_CALLBACK (on_close), w);
    g_object_set_data_full (G_OBJECT (w->win), "win", w, win_free);

    g_windows++;
    return w->win;
}

static void
on_ready_to_show (WebKitWebView *view, gpointer u)
{
    (void) u;
    gtk_window_present (GTK_WINDOW (window_new (view, FALSE)));
}

static void
view_wire (WebKitWebView *view)
{
    /* NB: in WebKitGTK 6.0 "download-started" only exists on the network
     * session, not on the web view - it is wired up once in main(). */
    g_signal_connect (view, "decide-policy",      G_CALLBACK (on_decide_policy), NULL);
    g_signal_connect (view, "permission-request", G_CALLBACK (on_permission), NULL);
    g_signal_connect (view, "create",             G_CALLBACK (on_create), NULL);
}

static GtkWidget *
on_create (WebKitWebView *view, WebKitNavigationAction *act, gpointer u)
{
    (void) act; (void) u;

    WebKitWebView *nv = g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                      "related-view",         view,
                                      "network-session",      g_session,
                                      "settings",             g_settings,
                                      "user-content-manager", g_ucm,
                                      NULL);
    webkit_web_view_set_zoom_level (nv, g_zoom);
    view_wire (nv);
    g_signal_connect (nv, "ready-to-show", G_CALLBACK (on_ready_to_show), NULL);
    return GTK_WIDGET (nv);
}

/* -------------------------------------------------------------- session */

static void
setup_session (void)
{
    if (g_private) {
        g_session = webkit_network_session_new_ephemeral ();
        LOG ("profile: private (ephemeral)\n");
        return;
    }

    char *data_dir = NULL, *cache_dir = NULL;
    profile_dirs (g_profile, &data_dir, &cache_dir);

    if (g_clear_data) {
        LOG ("clear-data: wiping profile data...\n");
        rm_rf (data_dir);
        rm_rf (cache_dir);
    }

    g_mkdir_with_parents (data_dir, 0700);
    g_mkdir_with_parents (cache_dir, 0700);

    g_session = webkit_network_session_new (data_dir, cache_dir);

    WebKitCookieManager *cm = webkit_network_session_get_cookie_manager (g_session);

    /* helps with some SSO / multi-domain auth flows */
    webkit_cookie_manager_set_accept_policy (cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);

    /* persist cookies so logins survive restarts */
    char *cookie_path = g_build_filename (data_dir, "cookies.sqlite", NULL);
    webkit_cookie_manager_set_persistent_storage (cm, cookie_path,
                                                  WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);

    LOG ("profile: name=%s\n", g_profile ? g_profile : "default");
    LOG ("profile: data=%s cache=%s cookies=%s\n", data_dir, cache_dir, cookie_path);

    g_free (cookie_path);
    g_free (data_dir);
    g_free (cache_dir);
}

static void
setup_settings (void)
{
    g_settings = webkit_settings_new ();

    /* best effort at behaving like a mainstream browser */
    webkit_settings_set_enable_site_specific_quirks (g_settings, TRUE);

    /* Cloudflare/Turnstile sometimes correlates missing GPU features with bots */
    settings_set_bool_if_exists (g_settings, "enable-webgl", TRUE);
    settings_set_bool_if_exists (g_settings, "enable-accelerated-2d-canvas", TRUE);

    if (g_user_agent && *g_user_agent) {
        webkit_settings_set_user_agent (g_settings, g_user_agent);
        LOG ("ua: %s\n", g_user_agent);
    }

    webkit_settings_set_enable_developer_extras                 (g_settings, TRUE);
    webkit_settings_set_enable_media_stream                     (g_settings, TRUE);
    webkit_settings_set_enable_webrtc                           (g_settings, TRUE);
    webkit_settings_set_enable_mediasource                      (g_settings, TRUE);
    webkit_settings_set_enable_encrypted_media                  (g_settings, TRUE);
    webkit_settings_set_enable_webaudio                         (g_settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture    (g_settings, FALSE);
    webkit_settings_set_javascript_can_access_clipboard         (g_settings, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout (g_settings, !g_quiet);
}

/* ----------------------------------------------------------------- main */

int
main (int argc, char **argv)
{
    const char *url_arg         = NULL;
    gboolean    open_devtools   = FALSE;
    gboolean    want_page_title = FALSE;

#define NEED_ARG(opt) \
    do { if (i + 1 >= argc) { \
             g_printerr ("%s: %s needs an argument\n", argv[0], opt); \
             return 1; \
         } } while (0)

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp (a, "-h") || !strcmp (a, "--help")) {
            usage (argv[0], TRUE);
            return 0;
        } else if (!strcmp (a, "-q") || !strcmp (a, "--quiet")) {
            g_quiet = TRUE;
        } else if (!strcmp (a, "--title")) {
            NEED_ARG ("--title");
            g_free (g_title);
            g_title = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--app-id")) {
            NEED_ARG ("--app-id");
            g_free (g_app_id);
            g_app_id = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--page-title")) {
            want_page_title = TRUE;
        } else if (!strcmp (a, "--mod")) {
            NEED_ARG ("--mod");
            if (!parse_mod (argv[++i])) {
                g_printerr ("%s: unknown --mod %s (ctrl|alt|super|meta)\n", argv[0], argv[i]);
                return 1;
            }
        } else if (!strcmp (a, "--clip-cmd")) {
            NEED_ARG ("--clip-cmd");
            g_free (g_clip_cmd);
            g_clip_cmd = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--zoom")) {
            NEED_ARG ("--zoom");
            g_zoom = CLAMP (g_ascii_strtod (argv[++i], NULL), ZOOM_MIN, ZOOM_MAX);
        } else if (!strcmp (a, "--css")) {
            NEED_ARG ("--css");
            g_css_path = argv[++i];
        } else if (!strcmp (a, "--devtools")) {
            open_devtools = TRUE;
        } else if (!strcmp (a, "--no-media")) {
            g_deny_media = TRUE;
        } else if (!strcmp (a, "--download-dir")) {
            NEED_ARG ("--download-dir");
            g_free (g_download_dir);
            g_download_dir = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--private")) {
            g_private = TRUE;
        } else if (!strcmp (a, "--profile")) {
            NEED_ARG ("--profile");
            g_free (g_profile);
            g_profile = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--clear-data")) {
            g_clear_data = TRUE;
        } else if (!strcmp (a, "--user-agent")) {
            NEED_ARG ("--user-agent");
            g_free (g_user_agent);
            g_user_agent = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--ua-chrome")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
        } else if (!strcmp (a, "--ua-win-chrome")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/127.0.0.0 Safari/537.36");
        } else if (!strcmp (a, "--ua-win-edge")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/127.0.0.0 Safari/537.36 Edg/127.0.0.0");
        } else if (!strcmp (a, "--ua-firefox")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
        } else if (!strcmp (a, "--ua-safari")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 Safari/605.1.15");
        } else if (a[0] != '-' && !url_arg) {
            url_arg = a;
        } else {
            g_printerr ("%s: unknown argument: %s\n", argv[0], a);
            usage_short (argv[0]);
            return 1;
        }
    }
#undef NEED_ARG

    if (!url_arg) {
        usage_short (argv[0]);
        return 1;
    }

    char *url = normalize_uri (url_arg);
    if (!url) {
        usage_short (argv[0]);
        return 1;
    }

    /* The page <title> drives the window title by default. An explicit
     * --title pins it instead, unless --page-title is also given - then
     * --title is just the fallback for pages with no title of their own. */
    g_follow_page_title = want_page_title || (g_title == NULL);

    if (!g_title)
        g_title = g_strdup (DEFAULT_TITLE);

    if (!g_app_id)
        g_app_id = g_strdup (DEFAULT_APP_ID);

    /* must happen before the first toplevel is created: GTK reads the
     * prgname when it builds the xdg_toplevel / sets WM_CLASS */
    g_set_prgname (g_app_id);
    g_set_application_name (g_title);

    if (!g_download_dir) {
        const char *d = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
        g_download_dir = g_strdup (d ? d : ".");
    }

    if (g_private && (g_profile || g_clear_data))
        LOG ("note: --private ignores --profile/--clear-data\n");

    /* a clipboard helper that dies early must not take us with it */
    signal (SIGPIPE, SIG_IGN);

    gtk_init ();
    ui_css_install ();

    setup_session ();
    g_signal_connect (g_session, "download-started",
                      G_CALLBACK (on_session_download_started), NULL);
    object_set_string_if_exists (G_OBJECT (g_session), "downloads-directory", g_download_dir);

    setup_settings ();

    g_ucm = webkit_user_content_manager_new ();
    css_reload ();

    WebKitWebView *view = g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                        "network-session",      g_session,
                                        "settings",             g_settings,
                                        "user-content-manager", g_ucm,
                                        NULL);
    webkit_web_view_set_zoom_level (view, g_zoom);
    view_wire (view);

    if (open_devtools)
        g_signal_connect (view, "load-changed", G_CALLBACK (on_load_changed), NULL);

    GtkWidget *win  = window_new (view, TRUE);
    GMainLoop *loop = g_main_loop_new (NULL, FALSE);
    g_object_set_data (G_OBJECT (win), "loop", loop);

    LOG ("load: %s\n", url);
    webkit_web_view_load_uri (view, url);
    gtk_window_present (GTK_WINDOW (win));

    g_main_loop_run (loop);

    g_main_loop_unref (loop);
    g_free (url);
    g_free (g_title);
    g_free (g_app_id);
    g_free (g_clip_cmd);
    g_free (g_download_dir);
    g_free (g_profile);
    g_free (g_user_agent);
    return 0;
}
