/* Installing and uninstalling AppImages.
 *
 * Release lookup follows tools/catalog.py (pick_latest); keep the two in step. */

#include <errno.h>
#include <string.h>
#include <sys/utsname.h>
#include <glib/gstdio.h>
#include <gio/gdesktopappinfo.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include "store-install.h"

#define EXTRACT_TIMEOUT_SECONDS 60
#define CHUNK (256 * 1024)

/* ---------- paths ---------- */

static char *
data_root (void)
{
  return g_build_filename (g_get_user_data_dir (), "sawmill", NULL);
}

char *
store_install_dir (StoreApp *a)
{
  return g_build_filename (g_get_user_data_dir (), "sawmill", a->id, NULL);
}

char *
store_install_appimage_path (StoreApp *a)
{
  g_autofree char *dir = store_install_dir (a);
  return g_build_filename (dir, "app.AppImage", NULL);
}

char *
store_install_desktop_path (StoreApp *a)
{
  g_autofree char *slug = g_strdup_printf ("sawmill-%s.desktop", a->id);
  for (char *p = slug; *p; p++)
    if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_' && *p != '.')
      *p = '-';
  return g_build_filename (g_get_user_data_dir (), "applications", slug, NULL);
}

void
store_install_load_state (StoreApp *a)
{
  g_autofree char *appimage = store_install_appimage_path (a);
  g_autofree char *dir = store_install_dir (a);
  g_autofree char *version_file = g_build_filename (dir, "version", NULL);
  char *version = NULL;

  g_clear_pointer (&a->installed_version, g_free);
  if (!g_file_test (appimage, G_FILE_TEST_IS_REGULAR))
    {
      a->state = APP_AVAILABLE;
      return;
    }
  a->state = APP_INSTALLED;
  if (g_file_get_contents (version_file, &version, NULL, NULL))
    a->installed_version = g_strstrip (version);
  /* an unknown installed version also counts as outdated once a release is known */
  if (a->latest_version && g_strcmp0 (a->installed_version, a->latest_version) != 0)
    a->state = APP_UPDATE;
}

static gboolean
remove_tree (const char *path)
{
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GFileEnumerator) en = g_file_enumerate_children (f, G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                                              G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
  GFileInfo *info;

  if (!g_file_test (path, G_FILE_TEST_EXISTS) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    return TRUE;
  if (!en)
    return g_unlink (path) == 0;
  while ((info = g_file_enumerator_next_file (en, NULL, NULL)))
    {
      g_autofree char *child = g_build_filename (path, g_file_info_get_name (info), NULL);
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        remove_tree (child);
      else
        g_unlink (child);
      g_object_unref (info);
    }
  return g_rmdir (path) == 0 || errno == ENOENT;
}

/* Remove dir if it is empty, then its empty owner/ and host/ folders (never above ~/.local/share/sawmill). */
static void
prune_empty (const char *dir)
{
  g_autofree char *root = data_root ();
  g_autofree char *current = g_strdup (dir);
  for (int i = 0; i < 3 && g_str_has_prefix (current, root) && !g_str_equal (current, root); i++)
    {
      char *up;
      if (g_rmdir (current) != 0)
        break;
      up = g_path_get_dirname (current);
      g_free (current);
      current = up;
    }
}

/* ---------- the job ---------- */

typedef struct {
  /* copied from the app on the main thread */
  GTask            *task;           /* not a reference: the task owns this Job */
  StoreApp         *app;
  char             *id, *host, *owner, *repo, *title, *summary;
  char             *catalog_icon;
  char             *asset_rx, *tag_pattern;
  gboolean          prereleases;
  char             *install_dir, *appimage, *desktop;
  StoreProgressFunc progress;
  gpointer          progress_data;
  double            last_reported;
} Job;

static void
job_free (Job *j)
{
  g_free (j->id); g_free (j->host); g_free (j->owner); g_free (j->repo); g_free (j->title); g_free (j->summary);
  g_free (j->catalog_icon); g_free (j->asset_rx); g_free (j->tag_pattern);
  g_free (j->install_dir); g_free (j->appimage); g_free (j->desktop);
  g_free (j);
}

typedef struct {
  Job    *job;
  GTask  *task;        /* keeps the Job alive until the report is delivered */
  char   *phase;
  double  fraction;
} Report;

static gboolean
report_idle (gpointer data)
{
  Report *r = data;
  if (r->job->progress)
    r->job->progress (r->job->app, r->phase, r->fraction, r->job->progress_data);
  return G_SOURCE_REMOVE;
}

static void
report_free (Report *r)
{
  g_object_unref (r->task);
  g_free (r->phase);
  g_free (r);
}

/* From the worker thread: deliver progress on the main thread. */
static void
report (Job *j, const char *phase, double fraction)
{
  Report *r = g_new0 (Report, 1);
  r->job = j;
  r->task = g_object_ref (j->task);
  r->phase = g_strdup (phase);
  r->fraction = fraction;
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT, report_idle, r, (GDestroyNotify) report_free);
}

static const char *
this_arch (void)
{
  static char machine[65];
  struct utsname u;
  if (!machine[0] && uname (&u) == 0)
    g_strlcpy (machine, u.machine, sizeof machine);
  if (g_str_equal (machine, "arm64"))
    return "aarch64";
  return machine[0] ? machine : "x86_64";
}

/* ---------- HTTP ---------- */

static SoupMessage *
new_request (const char *url, gboolean api)
{
  SoupMessage *msg = soup_message_new ("GET", url);
  SoupMessageHeaders *h;
  const char *token = g_getenv ("GITHUB_TOKEN");

  if (!msg)
    return NULL;
  h = soup_message_get_request_headers (msg);
  if (api)
    soup_message_headers_append (h, "Accept", "application/json");
  if (api && token && *token && g_str_has_prefix (url, "https://api.github.com/"))
    {
      g_autofree char *auth = g_strdup_printf ("Bearer %s", token);
      soup_message_headers_append (h, "Authorization", auth);
    }
  return msg;
}

static gboolean
check_status (SoupMessage *msg, const char *what, GError **error)
{
  guint status = soup_message_get_status (msg);
  SoupMessageHeaders *h = soup_message_get_response_headers (msg);
  const char *remaining = soup_message_headers_get_one (h, "x-ratelimit-remaining");

  if (SOUP_STATUS_IS_SUCCESSFUL (status))
    return TRUE;
  if ((status == 403 || status == 429) && (!remaining || g_str_equal (remaining, "0")))
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                 "The release server's request limit was reached. Try again in an hour, or set GITHUB_TOKEN");
  else if (status == 404)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s was not found (HTTP 404)", what);
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not get %s: HTTP %u %s", what, status,
                 soup_message_get_reason_phrase (msg) ? soup_message_get_reason_phrase (msg) : "");
  return FALSE;
}

static JsonNode *
get_json (SoupSession *s, const char *url, GCancellable *cancel, GError **error)
{
  g_autoptr (SoupMessage) msg = new_request (url, TRUE);
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();

  bytes = soup_session_send_and_read (s, msg, cancel, error);
  if (!bytes || !check_status (msg, "the list of releases", error))
    return NULL;
  if (!json_parser_load_from_data (parser, g_bytes_get_data (bytes, NULL), (gssize) g_bytes_get_size (bytes), error))
    return NULL;
  return json_node_copy (json_parser_get_root (parser));
}

/* ---------- releases ---------- */

typedef struct {
  char   *name, *url, *digest;
  gint64  size;
} Asset;

typedef struct {
  char      *tag, *date;
  gboolean   prerelease;
  GPtrArray *assets;   /* Asset* */
} Release;

static void asset_free (Asset *a) { g_free (a->name); g_free (a->url); g_free (a->digest); g_free (a); }
static void release_free (Release *r) { g_free (r->tag); g_free (r->date); g_ptr_array_unref (r->assets); g_free (r); }

static const char *
str_member (JsonObject *o, const char *name)
{
  JsonNode *n = json_object_get_member (o, name);
  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING ? json_node_get_string (n) : NULL;
}

static gint64
int_member (JsonObject *o, const char *name)
{
  JsonNode *n = json_object_get_member (o, name);
  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_INT64 ? json_node_get_int (n) : -1;
}

static JsonArray *
array_member (JsonObject *o, const char *name)
{
  JsonNode *n = o ? json_object_get_member (o, name) : NULL;
  return n && JSON_NODE_HOLDS_ARRAY (n) ? json_node_get_array (n) : NULL;
}

static JsonObject *
object_member (JsonObject *o, const char *name)
{
  JsonNode *n = o ? json_object_get_member (o, name) : NULL;
  return n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

static gboolean
bool_member (JsonObject *o, const char *name)
{
  JsonNode *n = json_object_get_member (o, name);
  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_BOOLEAN && json_node_get_boolean (n);
}

static char *
api_base (Job *j)
{
  if (g_str_equal (j->host, "github.com"))
    return g_strdup_printf ("https://api.github.com/repos/%s/%s", j->owner, j->repo);
  if (g_str_equal (j->host, "gitlab.com"))
    {
      g_autofree char *path = g_strdup_printf ("%s/%s", j->owner, j->repo);
      g_autofree char *enc = g_uri_escape_string (path, NULL, FALSE);
      return g_strdup_printf ("https://%s/api/v4/projects/%s", j->host, enc);
    }
  return g_strdup_printf ("https://%s/api/v1/repos/%s/%s", j->host, j->owner, j->repo);   /* Gitea, Codeberg */
}

static GPtrArray *
fetch_releases (SoupSession *s, Job *j, GCancellable *cancel, GError **error)
{
  g_autofree char *base = api_base (j);
  gboolean gitlab = g_str_equal (j->host, "gitlab.com");
  gboolean github = g_str_equal (j->host, "github.com");
  g_autofree char *url = g_strdup_printf ("%s/releases?%s=30", base, github || gitlab ? "per_page" : "limit");
  g_autoptr (JsonNode) root = get_json (s, url, cancel, error);
  GPtrArray *out;
  JsonArray *arr;

  if (!root)
    return NULL;
  if (!JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unexpected answer from the release server");
      return NULL;
    }
  arr = json_node_get_array (root);
  out = g_ptr_array_new_with_free_func ((GDestroyNotify) release_free);
  for (guint i = 0; i < json_array_get_length (arr); i++)
    {
      JsonObject *o = json_array_get_object_element (arr, i);
      Release *r;
      JsonArray *assets = NULL;

      if (!o || !str_member (o, "tag_name") || bool_member (o, "draft"))
        continue;
      r = g_new0 (Release, 1);
      r->tag = g_strdup (str_member (o, "tag_name"));
      r->date = g_strdup (str_member (o, gitlab ? "released_at" : "published_at") ?: "");
      r->prerelease = bool_member (o, gitlab ? "upcoming_release" : "prerelease");
      r->assets = g_ptr_array_new_with_free_func ((GDestroyNotify) asset_free);
      assets = gitlab ? array_member (object_member (o, "assets"), "links") : array_member (o, "assets");
      for (guint k = 0; assets && k < json_array_get_length (assets); k++)
        {
          JsonObject *ao = json_array_get_object_element (assets, k);
          const char *name = ao ? str_member (ao, "name") : NULL;
          const char *aurl = ao ? str_member (ao, gitlab ? "direct_asset_url" : "browser_download_url") : NULL;
          Asset *a;
          if (gitlab && ao && !aurl)
            aurl = str_member (ao, "url");
          if (!name || !aurl)
            continue;
          a = g_new0 (Asset, 1);
          a->name = g_strdup (name);
          a->url = g_strdup (aurl);
          a->digest = g_strdup (str_member (ao, "digest"));
          a->size = int_member (ao, "size");
          g_ptr_array_add (r->assets, a);
        }
      g_ptr_array_add (out, r);
    }
  return out;
}

static gboolean
rx_match (const char *pattern, const char *text, gboolean caseless)
{
  g_autoptr (GRegex) rx = g_regex_new (pattern, caseless ? G_REGEX_CASELESS : 0, 0, NULL);
  return rx && g_regex_match (rx, text, 0, NULL);
}

static const char *
asset_arch (const char *name)
{
  if (rx_match ("aarch64|arm64", name, TRUE))
    return "aarch64";
  if (rx_match ("x86[_-]64|amd64|x64", name, TRUE))
    return "x86_64";
  if (rx_match ("armhf|armv7|i[3-6]86|x86[_-]32|riscv|ppc64|s390x", name, TRUE))
    return NULL;
  return "x86_64";                       /* no architecture in the name */
}

static Asset *
pick_asset (Job *j, Release *r, const char *arch)
{
  Asset *best = NULL;
  for (guint i = 0; i < r->assets->len; i++)
    {
      Asset *a = g_ptr_array_index (r->assets, i);
      if (j->asset_rx)
        {
          if (rx_match (j->asset_rx, a->name, FALSE))
            return a;
          continue;
        }
      if (!g_str_has_suffix (a->name, ".AppImage") && !g_str_has_suffix (a->name, ".appimage"))
        continue;
      if (g_strcmp0 (asset_arch (a->name), arch) != 0)
        continue;
      if (!best)
        best = a;
      else
        {
          gboolean dbg_a = rx_match ("debug|dbg|symbols", a->name, TRUE), dbg_b = rx_match ("debug|dbg|symbols", best->name, TRUE);
          gsize la = strlen (a->name), lb = strlen (best->name);
          if (dbg_a < dbg_b || (dbg_a == dbg_b && (la < lb || (la == lb && strcmp (a->name, best->name) < 0))))
            best = a;
        }
    }
  return best;
}

static int
by_date_desc (gconstpointer pa, gconstpointer pb)
{
  return strcmp ((*(Release **) pb)->date, (*(Release **) pa)->date);
}

static Release *
pick_latest (Job *j, GPtrArray *releases, const char *arch, Asset **asset_out)
{
  static const char *version_tag = "^[A-Za-z_-]*\\d+(\\.\\d+)+[a-z]?$";
  g_autoptr (GPtrArray) usable = g_ptr_array_new ();
  gboolean any_version = FALSE;

  for (guint i = 0; i < releases->len; i++)
    {
      Release *r = g_ptr_array_index (releases, i);
      if (r->prerelease && !j->prereleases)
        continue;
      if (j->tag_pattern && !rx_match (j->tag_pattern, r->tag, FALSE))
        continue;
      g_ptr_array_add (usable, r);
      any_version |= rx_match (version_tag, r->tag, FALSE);
    }
  if (!j->tag_pattern && any_version)
    for (guint i = usable->len; i-- > 0;)
      if (!rx_match (version_tag, ((Release *) g_ptr_array_index (usable, i))->tag, FALSE))
        g_ptr_array_remove_index (usable, i);
  g_ptr_array_sort (usable, by_date_desc);

  for (guint i = 0; i < usable->len; i++)
    {
      Release *r = g_ptr_array_index (usable, i);
      Asset *a = pick_asset (j, r, arch);
      if (a)
        {
          *asset_out = a;
          return r;
        }
    }
  return NULL;
}

/* ---------- download ---------- */

static gboolean
download (SoupSession *s, Job *j, Asset *asset, const char *dest, GCancellable *cancel, GError **error)
{
  g_autoptr (SoupMessage) msg = new_request (asset->url, FALSE);
  g_autoptr (GInputStream) in = NULL;
  g_autoptr (GFile) file = g_file_new_for_path (dest);
  g_autoptr (GFileOutputStream) out = NULL;
  g_autoptr (GChecksum) sum = g_checksum_new (G_CHECKSUM_SHA256);
  g_autofree guchar *buf = g_malloc (CHUNK);
  gint64 total, done = 0;
  gssize n;

  in = soup_session_send (s, msg, cancel, error);
  if (!in || !check_status (msg, asset->name, error))
    return FALSE;
  total = soup_message_headers_get_content_length (soup_message_get_response_headers (msg));
  if (total <= 0)
    total = asset->size;
  out = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION, cancel, error);
  if (!out)
    return FALSE;

  while ((n = g_input_stream_read (in, buf, CHUNK, cancel, error)) > 0)
    {
      if (!g_output_stream_write_all (G_OUTPUT_STREAM (out), buf, (gsize) n, NULL, cancel, error))
        return FALSE;
      g_checksum_update (sum, buf, n);
      done += n;
      if (total > 0 && (double) done / total - j->last_reported >= 0.01)
        {
          j->last_reported = (double) done / total;
          report (j, "Downloading", 0.05 + 0.85 * j->last_reported);
        }
    }
  if (n < 0 || !g_output_stream_close (G_OUTPUT_STREAM (out), cancel, error))
    return FALSE;

  if (asset->digest && g_str_has_prefix (asset->digest, "sha256:") &&
      g_ascii_strcasecmp (asset->digest + 7, g_checksum_get_string (sum)) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "The download of %s is damaged: its checksum does not match the one published with the release",
                   asset->name);
      return FALSE;
    }
  if (done < 1024)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is only %" G_GINT64_FORMAT " bytes, not an AppImage",
                   asset->name, done);
      return FALSE;
    }
  return TRUE;
}

/* ---------- a clean environment for the programs we start ---------- */

/* When Sawmill itself runs as an AppImage, its runtime points these at Sawmill's bundled libraries
 * and data. Apps we unpack or launch must not inherit them, or their own GTK may break. */
static const char *const leaky_vars[] = {
  "APPIMAGE", "APPDIR", "ARGV0", "OWD", "SHARUN_DIR",
  "LD_LIBRARY_PATH", "LD_PRELOAD",
  "GSETTINGS_SCHEMA_DIR", "GDK_PIXBUF_MODULE_FILE", "GDK_PIXBUF_MODULEDIR",
  "GTK_PATH", "GTK_EXE_PREFIX", "GTK_DATA_PREFIX", "GTK_IM_MODULE_FILE", "GTK_THEME",
  "GIO_MODULE_DIR", "GIO_EXTRA_MODULES", "GI_TYPELIB_PATH",
  "GSK_RENDERER", "GDK_DISABLE", "GDK_GL",
  "PYTHONHOME", "PYTHONPATH", "PERLLIB", "QT_PLUGIN_PATH",
  NULL
};

/* A colon-separated list without entries inside our own AppImage, or NULL if nothing is left. */
static char *
clean_path_list (const char *var)
{
  const char *appdir = g_getenv ("APPDIR");
  const char *dirs = g_getenv (var);
  g_auto (GStrv) parts = NULL;
  g_autoptr (GStrvBuilder) keep = g_strv_builder_new ();
  g_auto (GStrv) kept = NULL;

  if (!dirs)
    return NULL;
  if (!appdir || !*appdir)
    return g_strdup (dirs);
  parts = g_strsplit (dirs, ":", -1);
  for (guint i = 0; parts[i]; i++)
    if (*parts[i] && !g_str_has_prefix (parts[i], appdir))
      g_strv_builder_add (keep, parts[i]);
  kept = g_strv_builder_end (keep);
  return kept[0] ? g_strjoinv (":", kept) : NULL;
}

/* Calls set(name, value) for each variable to change; value NULL means unset. */
static void
clean_env (void (*set) (gpointer target, const char *name, const char *value), gpointer target)
{
  g_autofree char *data_dirs = clean_path_list ("XDG_DATA_DIRS");
  g_autofree char *path = clean_path_list ("PATH");
  g_autofree char *cache = store_real_xdg_cache_home ();     /* undo the runtime's cache redirect */

  for (guint i = 0; leaky_vars[i]; i++)
    set (target, leaky_vars[i], NULL);
  set (target, "XDG_DATA_DIRS", data_dirs);
  if (path)
    set (target, "PATH", path);
  if (cache)
    set (target, "XDG_CACHE_HOME", cache);
}

static void
set_launcher (gpointer l, const char *name, const char *value)
{
  if (value)
    g_subprocess_launcher_setenv (l, name, value, TRUE);
  else
    g_subprocess_launcher_unsetenv (l, name);
}

static void
set_context (gpointer ctx, const char *name, const char *value)
{
  if (value)
    g_app_launch_context_setenv (ctx, name, value);
  else
    g_app_launch_context_unsetenv (ctx, name);
}

static void clean_launcher_env (GSubprocessLauncher *l) { clean_env (set_launcher, l); }
static void clean_launch_context (GAppLaunchContext *ctx) { clean_env (set_context, ctx); }

/* GNOME finds the apps for a file type through mimeinfo.cache in the launcher's folder, not the
 * launchers' MimeType lines, so rebuild it after adding or removing one. Best effort, not waited for:
 * without update-desktop-database the app still installs, it just is not offered for its file types. */
static void
refresh_mime_cache (const char *desktop_file)
{
  g_autoptr (GSubprocessLauncher) l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_autoptr (GSubprocess) proc = NULL;
  g_autofree char *dir = g_path_get_dirname (desktop_file);

  clean_launcher_env (l);
  proc = g_subprocess_launcher_spawn (l, NULL, "update-desktop-database", "-q", dir, NULL);
}

/* ---------- extracting the .desktop file and icon ---------- */

static void
on_waited (GObject *src, GAsyncResult *res, gpointer data)
{
  g_subprocess_wait_finish (G_SUBPROCESS (src), res, NULL);
  *(gboolean *) data = TRUE;
}

static gboolean
on_timeout (gpointer data)
{
  *(gboolean *) data = TRUE;
  return G_SOURCE_REMOVE;
}

/* `<appimage> --appimage-extract <pattern>` in cwd, killed after a timeout. Unpacks into cwd/squashfs-root. */
static gboolean
extract (const char *appimage, const char *cwd, const char *pattern, GCancellable *cancel, GError **error)
{
  g_autoptr (GSubprocessLauncher) l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_autoptr (GSubprocess) proc = NULL;
  g_autoptr (GMainContext) ctx = g_main_context_new ();
  g_autoptr (GSource) timeout = g_timeout_source_new_seconds (EXTRACT_TIMEOUT_SECONDS);
  gboolean exited = FALSE, timed_out = FALSE;

  g_subprocess_launcher_set_cwd (l, cwd);
  clean_launcher_env (l);
  proc = g_subprocess_launcher_spawn (l, error, appimage, "--appimage-extract", pattern, NULL);
  if (!proc)
    return FALSE;

  g_main_context_push_thread_default (ctx);
  g_subprocess_wait_async (proc, cancel, on_waited, &exited);
  g_source_set_callback (timeout, on_timeout, &timed_out, NULL);
  g_source_attach (timeout, ctx);
  while (!exited && !timed_out)
    g_main_context_iteration (ctx, TRUE);
  g_source_destroy (timeout);
  if (g_cancellable_is_cancelled (cancel))
    g_subprocess_force_exit (proc);                   /* wait_async returned early; stop the process too */
  if (!exited)
    {
      g_subprocess_force_exit (proc);
      while (!exited)
        g_main_context_iteration (ctx, TRUE);
    }
  g_main_context_pop_thread_default (ctx);

  if (timed_out)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                           "The AppImage did not unpack its files (it may be an old type 1 AppImage)");
      return FALSE;
    }
  return g_cancellable_set_error_if_cancelled (cancel, error) ? FALSE : TRUE;
}

static char *
find_root_desktop (const char *root)
{
  g_autoptr (GDir) dir = g_dir_open (root, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir)))
    if (g_str_has_suffix (name, ".desktop"))
      return g_build_filename (root, name, NULL);
  return NULL;
}

/* If path is a symlink inside root, unpack its target too and return the resolved path. */
static char *
resolve_in_root (const char *appimage, const char *work, const char *root, const char *path, GCancellable *cancel)
{
  g_autofree char *current = g_strdup (path);

  for (int hops = 0; hops < 5 && g_file_test (current, G_FILE_TEST_IS_SYMLINK); hops++)
    {
      g_autofree char *target = g_file_read_link (current, NULL);
      g_autofree char *dir = g_path_get_dirname (current);
      g_autofree char *joined = NULL;
      g_autofree char *canon = NULL;
      if (!target)
        return NULL;
      joined = g_path_is_absolute (target) ? g_build_filename (root, target, NULL) : g_build_filename (dir, target, NULL);
      canon = g_canonicalize_filename (joined, NULL);
      if (!g_str_has_prefix (canon, root))
        return NULL;                                   /* points outside the AppImage */
      if (!g_file_test (canon, G_FILE_TEST_EXISTS))
        extract (appimage, work, canon + strlen (root) + 1, cancel, NULL);
      g_free (current);
      current = g_steal_pointer (&canon);
    }
  return g_file_test (current, G_FILE_TEST_IS_REGULAR) ? g_steal_pointer (&current) : NULL;
}

static const char *
image_extension (const char *path)
{
  g_autofree char *head = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &head, &len, NULL) || len < 8)
    return NULL;
  if (memcmp (head, "\x89PNG", 4) == 0)
    return "png";
  if (g_strstr_len (head, MIN (len, 512), "<svg") || g_str_has_prefix (head, "<?xml"))
    return "svg";
  return NULL;
}

/* Unpack the AppImage's .desktop file and its icon. Returns the parsed desktop file (or NULL) and the
 * icon path (or NULL), both inside work/. */
static GKeyFile *
read_appimage_metadata (Job *j, const char *work, char **icon_out, GCancellable *cancel)
{
  g_autofree char *root = g_build_filename (work, "squashfs-root", NULL);
  g_autofree char *desktop = NULL;
  g_autoptr (GKeyFile) kf = NULL;
  g_autofree char *icon_name = NULL;

  *icon_out = NULL;
  if (!extract (j->appimage, work, "*.desktop", cancel, NULL) || !(desktop = find_root_desktop (root)))
    return NULL;
  kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, desktop, G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
    return NULL;

  /* Our launcher has a different file name than the app's own .desktop file, so GNOME would not
   * match the app's window to it. Apps usually use their desktop file name as their window id. */
  if (!g_key_file_has_key (kf, G_KEY_FILE_DESKTOP_GROUP, "StartupWMClass", NULL))
    {
      g_autofree char *base = g_path_get_basename (desktop);
      base[strlen (base) - strlen (".desktop")] = '\0';
      g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, "StartupWMClass", base);
    }

  icon_name = g_key_file_get_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
  if (icon_name && !strchr (icon_name, '/'))
    {
      const char *tries[][2] = {
        { "%s.svg", NULL }, { "%s.png", NULL }, { ".DirIcon", NULL },
        { "usr/share/icons/hicolor/scalable/apps/%s.svg", NULL },
        { "usr/share/icons/hicolor/512x512/apps/%s.png", NULL },
        { "usr/share/icons/hicolor/256x256/apps/%s.png", NULL },
        { "usr/share/icons/hicolor/128x128/apps/%s.png", NULL },
      };
      for (guint i = 0; i < G_N_ELEMENTS (tries) && !*icon_out; i++)
        {
          g_autofree char *rel = strstr (tries[i][0], "%s") ? g_strdup_printf (tries[i][0], icon_name)
                                                             : g_strdup (tries[i][0]);
          g_autofree char *path = g_build_filename (root, rel, NULL);
          g_autofree char *resolved = NULL;
          if (!g_file_test (path, G_FILE_TEST_EXISTS) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
            extract (j->appimage, work, rel, cancel, NULL);
          resolved = resolve_in_root (j->appimage, work, root, path, cancel);
          if (resolved && image_extension (resolved))
            *icon_out = g_steal_pointer (&resolved);
        }
    }
  return g_steal_pointer (&kf);
}

/* ---------- writing the launcher ---------- */

/* Quote a path for an Exec line (Desktop Entry spec: double quotes, escape " ` $ \). */
static char *
exec_quote (const char *path)
{
  GString *s = g_string_new ("\"");
  for (const char *p = path; *p; p++)
    {
      if (strchr ("\"`$\\", *p))
        g_string_append_c (s, '\\');
      g_string_append_c (s, *p);
    }
  g_string_append_c (s, '"');
  return g_string_free (s, FALSE);
}

/* Replace the program in an Exec line with the AppImage, keeping its arguments and field codes. */
static char *
rewrite_exec (const char *exec, const char *appimage)
{
  g_autofree char *quoted = exec_quote (appimage);
  const char *rest = "";

  if (exec)
    {
      const char *p = exec;
      while (*p == ' ')
        p++;
      if (*p == '"')
        {
          for (p++; *p && *p != '"'; p++)
            if (*p == '\\' && p[1])
              p++;
          if (*p)
            p++;
        }
      else
        while (*p && *p != ' ')
          p++;
      rest = p;
      while (*rest == ' ')
        rest++;
    }
  return *rest ? g_strdup_printf ("%s %s", quoted, rest) : g_strdup_printf ("%s %%U", quoted);
}

static gboolean
write_launcher (Job *j, GKeyFile *from_appimage, const char *icon, GError **error)
{
  g_autoptr (GKeyFile) kf = from_appimage ? g_key_file_ref (from_appimage) : g_key_file_new ();
  const char *G = G_KEY_FILE_DESKTOP_GROUP;
  g_auto (GStrv) groups = NULL;
  g_autofree char *dir = g_path_get_dirname (j->desktop);

  /* fill in what the AppImage left out or empty */
  {
    g_autofree char *name = g_key_file_get_string (kf, G, G_KEY_FILE_DESKTOP_KEY_NAME, NULL);
    g_autofree char *comment = g_key_file_get_string (kf, G, G_KEY_FILE_DESKTOP_KEY_COMMENT, NULL);
    if (!name || !*g_strstrip (name))
      g_key_file_set_string (kf, G, G_KEY_FILE_DESKTOP_KEY_NAME, j->title);
    if (!comment || !*g_strstrip (comment))
      g_key_file_set_string (kf, G, G_KEY_FILE_DESKTOP_KEY_COMMENT, j->summary);
  }
  g_key_file_set_string (kf, G, G_KEY_FILE_DESKTOP_KEY_TYPE, "Application");
  g_key_file_set_string (kf, G, G_KEY_FILE_DESKTOP_KEY_ICON, icon);
  g_key_file_set_string (kf, G, G_KEY_FILE_DESKTOP_KEY_TRY_EXEC, j->appimage);
  g_key_file_set_string (kf, G, "X-Sawmill-Id", j->id);
  g_key_file_remove_key (kf, G, "DBusActivatable", NULL);        /* only the AppImage can start it */

  /* every Exec line (main entry and actions) points at the installed AppImage */
  groups = g_key_file_get_groups (kf, NULL);
  for (guint i = 0; groups[i]; i++)
    {
      g_auto (GStrv) keys = NULL;
      if (!g_str_equal (groups[i], G) && !g_str_has_prefix (groups[i], "Desktop Action "))
        continue;
      if (g_key_file_has_key (kf, groups[i], G_KEY_FILE_DESKTOP_KEY_EXEC, NULL) || g_str_equal (groups[i], G))
        {
          g_autofree char *old = g_key_file_get_string (kf, groups[i], G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
          g_autofree char *exec = rewrite_exec (old, j->appimage);
          g_key_file_set_string (kf, groups[i], G_KEY_FILE_DESKTOP_KEY_EXEC, exec);
        }
      keys = g_key_file_get_keys (kf, groups[i], NULL, NULL);
      for (guint k = 0; keys && keys[k]; k++)
        if (g_str_has_prefix (keys[k], "X-AppImage"))
          g_key_file_remove_key (kf, groups[i], keys[k], NULL);
    }

  g_mkdir_with_parents (dir, 0755);
  return g_key_file_save_to_file (kf, j->desktop, error);
}

static gboolean
copy_file (const char *from, const char *to, GError **error)
{
  g_autoptr (GFile) a = g_file_new_for_path (from);
  g_autoptr (GFile) b = g_file_new_for_path (to);
  return g_file_copy (a, b, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error);
}

static Job *job_new (StoreApp *a, GTask *task);

/* ---------- the worker ---------- */

static void
install_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  Job *j = data;
  g_autoptr (SoupSession) s = soup_session_new_with_options ("user-agent", "Sawmill/0.1 (AppImage store)",
                                                             "timeout", 60, NULL);
  g_autoptr (GPtrArray) releases = NULL;
  g_autofree char *part = g_strconcat (j->appimage, ".part", NULL);
  g_autofree char *work = g_build_filename (j->install_dir, ".unpack", NULL);
  g_autofree char *icon_src = NULL;
  g_autofree char *icon_dest = NULL;
  g_autofree char *version_file = g_build_filename (j->install_dir, "version", NULL);
  g_autoptr (GKeyFile) meta = NULL;
  const char *arch = this_arch ();
  Release *rel;
  Asset *asset = NULL;
  GError *error = NULL;

  report (j, "Looking up the latest release", -1);
  if (!(releases = fetch_releases (s, j, cancel, &error)))
    goto fail;
  if (!(rel = pick_latest (j, releases, arch, &asset)))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "None of the %u most recent releases has an AppImage for this computer (%s)", releases->len, arch);
      goto fail;
    }

  if (g_mkdir_with_parents (j->install_dir, 0755) != 0)
    {
      g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno), "Cannot create %s: %s", j->install_dir, g_strerror (errno));
      goto fail;
    }
  report (j, "Downloading", 0.05);
  if (!download (s, j, asset, part, cancel, &error))
    goto fail;
  if (g_chmod (part, 0755) != 0 || g_rename (part, j->appimage) != 0)
    {
      g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno), "Cannot save the AppImage: %s", g_strerror (errno));
      goto fail;
    }

  report (j, "Adding to applications", 0.95);
  remove_tree (work);
  g_mkdir_with_parents (work, 0700);
  meta = read_appimage_metadata (j, work, &icon_src, cancel);
  if (g_cancellable_set_error_if_cancelled (cancel, &error))
    goto fail;
  if (!icon_src)
    icon_src = g_strdup (j->catalog_icon);              /* no icon inside: use the catalog's */
  {
    const char *ext = image_extension (icon_src);
    g_autofree char *old_png = g_build_filename (j->install_dir, "icon.png", NULL);
    g_autofree char *old_svg = g_build_filename (j->install_dir, "icon.svg", NULL);
    g_autofree char *name = g_strdup_printf ("icon.%s", ext ? ext : "png");
    g_unlink (old_png);
    g_unlink (old_svg);
    icon_dest = g_build_filename (j->install_dir, name, NULL);
  }
  if (!copy_file (icon_src, icon_dest, &error) || !write_launcher (j, meta, icon_dest, &error))
    goto fail;
  refresh_mime_cache (j->desktop);
  remove_tree (work);
  g_file_set_contents (version_file, rel->tag, -1, NULL);

  g_task_return_pointer (task, g_strdup (rel->tag), g_free);
  return;

fail:
  g_unlink (part);
  remove_tree (work);
  if (!g_file_test (j->appimage, G_FILE_TEST_EXISTS))
    prune_empty (j->install_dir);                    /* failed first install: leave nothing behind */
  g_task_return_error (task, error);
}

void
store_install_async (StoreApp *a, GCancellable *cancellable, StoreProgressFunc progress, gpointer progress_data,
                     GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  Job *j = job_new (a, task);

  j->progress = progress;
  j->progress_data = progress_data;
  g_task_set_task_data (task, j, (GDestroyNotify) job_free);
  g_task_set_source_tag (task, store_install_async);
  g_task_run_in_thread (task, install_thread);
}

static Job *
job_new (StoreApp *a, GTask *task)
{
  Job *j = g_new0 (Job, 1);
  const char *arch = this_arch ();

  j->task = task;
  j->app = a;
  j->id = g_strdup (a->id);
  j->host = g_strdup (a->host);
  j->owner = g_strdup (a->owner);
  j->repo = g_strdup (a->repo);
  j->title = g_strdup (a->title);
  j->summary = g_strdup (a->summary);
  j->catalog_icon = g_build_filename (a->dir, "icon.png", NULL);
  j->asset_rx = g_strdup (g_str_equal (arch, "aarch64") ? a->asset_aarch64 : a->asset_x86_64);
  j->tag_pattern = g_strdup (a->tag_pattern);
  j->prereleases = a->prereleases;
  j->install_dir = store_install_dir (a);
  j->appimage = store_install_appimage_path (a);
  j->desktop = store_install_desktop_path (a);
  return j;
}

/* ---------- update checks ---------- */

static void
check_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  Job *j = data;
  g_autoptr (SoupSession) s = soup_session_new_with_options ("user-agent", "Sawmill/0.1 (AppImage store)",
                                                             "timeout", 30, NULL);
  g_autoptr (GPtrArray) releases = NULL;
  const char *arch = this_arch ();
  Release *rel;
  Asset *asset = NULL;
  GError *error = NULL;

  if (!(releases = fetch_releases (s, j, cancel, &error)))
    {
      g_task_return_error (task, error);
      return;
    }
  if (!(rel = pick_latest (j, releases, arch, &asset)))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "No release with an AppImage for this computer (%s)", arch);
      return;
    }
  g_task_return_pointer (task, g_strdup (rel->tag), g_free);
}

void
store_check_update_async (StoreApp *a, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_task_data (task, job_new (a, task), (GDestroyNotify) job_free);
  g_task_set_source_tag (task, store_check_update_async);
  g_task_run_in_thread (task, check_thread);
}

char *
store_check_update_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

char *
store_install_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------- uninstall and launch ---------- */

gboolean
store_uninstall (StoreApp *a, GError **error)
{
  g_autofree char *desktop = store_install_desktop_path (a);
  g_autofree char *dir = store_install_dir (a);
  g_autofree char *parent = NULL;

  if (g_unlink (desktop) != 0 && errno != ENOENT)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Cannot remove %s: %s", desktop, g_strerror (errno));
      return FALSE;
    }
  refresh_mime_cache (desktop);
  if (!remove_tree (dir))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot remove %s", dir);
      return FALSE;
    }
  parent = g_path_get_dirname (dir);
  prune_empty (parent);                              /* empty owner/ and host/ folders */
  store_install_load_state (a);
  return TRUE;
}

gboolean
store_launch (StoreApp *a, GError **error)
{
  g_autofree char *desktop = store_install_desktop_path (a);
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new_from_filename (desktop);
  g_autoptr (GdkAppLaunchContext) ctx = gdk_display_get_app_launch_context (gdk_display_get_default ());

  clean_launch_context (G_APP_LAUNCH_CONTEXT (ctx));
  if (info)
    return g_app_info_launch (G_APP_INFO (info), NULL, G_APP_LAUNCH_CONTEXT (ctx), error);

  {
    g_autofree char *appimage = store_install_appimage_path (a);
    g_autoptr (GSubprocessLauncher) l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
    g_autoptr (GSubprocess) proc = NULL;
    clean_launcher_env (l);
    proc = g_subprocess_launcher_spawn (l, error, appimage, NULL);
    return proc != NULL;
  }
}
