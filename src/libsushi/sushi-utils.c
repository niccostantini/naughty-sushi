/* SPDX-License-Identifier: GPL-2.0-or-later WITH GStreamer-exception-2008
 * SPDX-FileCopyrightText: 2011 Red Hat, Inc.
 *
 * Authors: Cosimo Cecchi <cosimoc@redhat.com>
 */

#include "sushi-utils.h"

#include <glib/gstdio.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include <gst/tag/tag.h>
#include <gst/pbutils/pbutils.h>

#include "externalwindow.h"
#define I_KNOW_THE_PAPERS_LIBS_ARE_UNSTABLE_AND_HAVE_TALKED_WITH_THE_AUTHORS
#include <papers-view.h>
#include <papers-document.h>


void
sushi_window_set_child_of_external (GtkWindow *window,
                                    const char *handle)
{
  ExternalWindow *external_window;

  gtk_widget_realize (GTK_WIDGET (window));

  external_window = create_external_window_from_handle (handle);
  if (!external_window)
    return;

  external_window_set_parent_of (external_window, window);
  g_object_unref (external_window);
}

static void load_libreoffice (GTask *task);

typedef struct {
  GFile *file;
  gchar *pdf_path;
  gchar *x2t_params_path;

  gboolean checked_libreoffice_flatpak;
  gboolean have_libreoffice_flatpak;
} TaskData;

static void
task_data_free (TaskData *data)
{
  if (data->pdf_path) {
    g_unlink (data->pdf_path);
    g_free (data->pdf_path);
  }

  if (data->x2t_params_path) {
    g_unlink (data->x2t_params_path);
    g_free (data->x2t_params_path);
  }

  g_clear_object (&data->file);
  g_free (data);
}

static void
libreoffice_missing_ready_cb (GObject *source,
                              GAsyncResult *res,
                              gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;

  g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (error != NULL) {
    /* can't install libreoffice with packagekit - nothing else we can do */
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  /* now that we have libreoffice installed, try again loading the document */
  load_libreoffice (task);
}

static void
libreoffice_missing (GTask *task)
{
  GApplication *app = g_application_get_default ();
  GDBusConnection *connection = g_application_get_dbus_connection (app);
  const gchar *libreoffice_path[2];

  libreoffice_path[0] = "/usr/bin/libreoffice";
  libreoffice_path[1] = NULL;

  g_dbus_connection_call (connection,
                          "org.freedesktop.PackageKit",
                          "/org/freedesktop/PackageKit",
                          "org.freedesktop.PackageKit.Modify2",
                          "InstallProvideFiles",
                          g_variant_new ("(^asssa{sv})",
                                         libreoffice_path,
                                         "hide-confirm-deps",
                                         "io.github.niccostantini.Naughty.Previewer",
                                         NULL),
                          NULL, G_DBUS_CALL_FLAGS_NONE,
                          G_MAXINT, NULL,
                          libreoffice_missing_ready_cb,
                          g_object_ref (task));
}

static void
libreoffice_done_cb (GObject      *object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  GTask *task = user_data;
  g_autoptr(GError) error = NULL;
  TaskData *data = g_task_get_task_data (task);
  GSubprocess *subprocess = G_SUBPROCESS (object);

  if (!g_subprocess_wait_finish (subprocess, res, &error))
  {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_subprocess_force_exit (subprocess);
    else
      g_warning ("Error converting libreoffice file: %s", error->message);
  }

  g_task_return_pointer (task, g_file_new_for_path (data->pdf_path), g_object_unref);
}

#define LIBREOFFICE_FLATPAK "org.libreoffice.LibreOffice"

static gboolean
check_libreoffice_flatpak (GTask       *task,
                           const gchar *flatpak_path)
{
  const gchar *check_argv[] = { flatpak_path, "info", LIBREOFFICE_FLATPAK, NULL };
  g_autoptr(GError) error = NULL;
  gboolean ret;
  gint wait_status = -1;
  TaskData *data = g_task_get_task_data (task);

  if (data->checked_libreoffice_flatpak)
    return data->have_libreoffice_flatpak;

  data->checked_libreoffice_flatpak = TRUE;

  ret = g_spawn_sync (NULL, (gchar **) check_argv, NULL,
                      G_SPAWN_DEFAULT |
                      G_SPAWN_STDERR_TO_DEV_NULL |
                      G_SPAWN_STDOUT_TO_DEV_NULL,
                      NULL, NULL,
                      NULL, NULL,
                      &wait_status, &error);

  if (ret) {
    g_autoptr(GError) child_error = NULL;
    if (g_spawn_check_wait_status (wait_status, &child_error)) {
      g_debug ("Found LibreOffice flatpak!");
      data->have_libreoffice_flatpak = TRUE;
    } else {
      g_debug ("LibreOffice flatpak not found, flatpak info returned %i (%s)",
               wait_status, child_error->message);
    }
  } else {
    g_warning ("Error while checking for LibreOffice flatpak: %s",
               error->message);
  }

  return data->have_libreoffice_flatpak;
}

/* OnlyOffice Desktop Editors ship a headless converter, x2t. It needs the
 * font index that the desktop app writes on its first start. */
static const gchar *onlyoffice_x2t_paths[] = {
  "/snap/onlyoffice-desktopeditors/current/opt/onlyoffice/desktopeditors/converter/x2t",
  "/opt/onlyoffice/desktopeditors/converter/x2t",
};

static gchar *
find_onlyoffice_fonts_dir (void)
{
  g_autofree gchar *snap_dir = g_build_filename (g_get_home_dir (), "snap",
                                                 "onlyoffice-desktopeditors", "current",
                                                 ".local", "share", "onlyoffice",
                                                 "desktopeditors", "data", "fonts", NULL);
  g_autofree gchar *native_dir = g_build_filename (g_get_user_data_dir (), "onlyoffice",
                                                   "desktopeditors", "data", "fonts", NULL);
  const gchar *dirs[] = { snap_dir, native_dir };

  for (gsize i = 0; i < G_N_ELEMENTS (dirs); i++) {
    g_autofree gchar *all_fonts = g_build_filename (dirs[i], "AllFonts.js", NULL);

    if (g_file_test (all_fonts, G_FILE_TEST_IS_REGULAR))
      return g_strdup (dirs[i]);
  }

  return NULL;
}

static gboolean
load_onlyoffice (GTask       *task,
                 const gchar *doc_path,
                 const gchar *pdf_dir)
{
  TaskData *data = g_task_get_task_data (task);
  const gchar *x2t_path = NULL;
  g_autofree gchar *fonts_dir = NULL;
  g_autofree gchar *all_fonts = NULL;
  g_autofree gchar *temp_dir = NULL;
  g_autofree gchar *params = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) subprocess = NULL;

  for (gsize i = 0; i < G_N_ELEMENTS (onlyoffice_x2t_paths); i++) {
    if (g_file_test (onlyoffice_x2t_paths[i], G_FILE_TEST_IS_EXECUTABLE)) {
      x2t_path = onlyoffice_x2t_paths[i];
      break;
    }
  }

  fonts_dir = find_onlyoffice_fonts_dir ();
  if (x2t_path == NULL || fonts_dir == NULL)
    return FALSE;

  all_fonts = g_build_filename (fonts_dir, "AllFonts.js", NULL);
  temp_dir = g_build_filename (pdf_dir, "x2t-tmp", NULL);
  g_mkdir_with_parents (temp_dir, 0700);

  /* 513 is x2t's code for PDF output */
  params = g_markup_printf_escaped ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                    "<TaskQueueDataConvert>"
                                    "<m_sFileFrom>%s</m_sFileFrom>"
                                    "<m_sFileTo>%s</m_sFileTo>"
                                    "<m_nFormatTo>513</m_nFormatTo>"
                                    "<m_sTempDir>%s</m_sTempDir>"
                                    "<m_sAllFontsPath>%s</m_sAllFontsPath>"
                                    "<m_sFontDir>%s</m_sFontDir>"
                                    "</TaskQueueDataConvert>",
                                    doc_path, data->pdf_path, temp_dir,
                                    all_fonts, fonts_dir);

  g_clear_pointer (&data->x2t_params_path, g_free);
  data->x2t_params_path = g_strconcat (data->pdf_path, ".x2t.xml", NULL);
  if (!g_file_set_contents (data->x2t_params_path, params, -1, &error)) {
    g_warning ("Error writing OnlyOffice converter parameters: %s", error->message);
    return FALSE;
  }

  g_debug ("Converting with OnlyOffice: %s %s", x2t_path, data->x2t_params_path);

  subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                 &error, x2t_path, data->x2t_params_path, NULL);
  if (subprocess == NULL) {
    g_warning ("Error while spawning OnlyOffice converter: %s", error->message);
    return FALSE;
  }

  g_subprocess_wait_async (subprocess, g_task_get_cancellable (task),
                           libreoffice_done_cb, task);
  return TRUE;
}

static void
load_libreoffice (GTask *task)
{
  g_autofree gchar *flatpak_path = NULL, *libreoffice_path = NULL;
  g_autofree gchar *doc_path = NULL, *doc_name = NULL, *tmp_name = NULL;
  g_autofree gchar *command = NULL, *pdf_dir = NULL;
  g_auto(GStrv) argv = NULL;
  g_autoptr(GError) error = NULL;
  gboolean use_flatpak = FALSE;
  g_autoptr(GSubprocess) subprocess = NULL;
  GCancellable *cancellable = g_task_get_cancellable (task);
  TaskData *data = g_task_get_task_data (task);

  doc_path = g_file_get_path (data->file);
  doc_name = g_file_get_basename (data->file);

  /* libreoffice --convert-to replaces the extension with .pdf */
  tmp_name = g_strrstr (doc_name, ".");
  if (tmp_name)
    *tmp_name = '\0';
  tmp_name = g_strdup_printf ("%s.pdf", doc_name);

  pdf_dir = g_build_filename (g_get_user_cache_dir (), "sushi", NULL);
  g_clear_pointer (&data->pdf_path, g_free);
  data->pdf_path = g_build_filename (pdf_dir, tmp_name, NULL);
  g_mkdir_with_parents (pdf_dir, 0700);

  flatpak_path = g_find_program_in_path ("flatpak");
  if (flatpak_path != NULL)
    use_flatpak = check_libreoffice_flatpak (task, flatpak_path);

  if (!use_flatpak) {
    libreoffice_path = g_find_program_in_path ("libreoffice");
    if (libreoffice_path == NULL) {
      if (!load_onlyoffice (task, doc_path, pdf_dir))
        libreoffice_missing (task);
      return;
    }
  }

  if (use_flatpak) {
    g_autofree gchar *flatpak_doc = g_strdup_printf ("--filesystem=%s:ro", doc_path);
    g_autofree gchar *flatpak_dir = g_strdup_printf ("--filesystem=%s", pdf_dir);

    const gchar *flatpak_argv[] = {
      NULL, /* to be replaced with flatpak binary */
      "run", "--command=/app/libreoffice/program/soffice",
      "--nofilesystem=host",
      NULL, /* to be replaced with filesystem permissions to read document */
      NULL, /* to be replaced with filesystem permissions to write output */
      LIBREOFFICE_FLATPAK,
      "--convert-to", "pdf",
      "--outdir", NULL, /* to be replaced with output dir */
      NULL, /* to be replaced with input file */
      NULL
    };

    flatpak_argv[0] = flatpak_path;
    flatpak_argv[4] = flatpak_doc;
    flatpak_argv[5] = flatpak_dir;
    flatpak_argv[10] = pdf_dir;
    flatpak_argv[11] = doc_path;

    argv = g_strdupv ((gchar **) flatpak_argv);
  } else {
    const gchar *libreoffice_argv[] = {
      NULL, /* to be replaced with binary */
      "--convert-to", "pdf",
      "--outdir", NULL, /* to be replaced with output dir */
      NULL, /* to be replaced with input file */
      NULL
    };

    libreoffice_argv[0] = libreoffice_path;
    libreoffice_argv[4] = pdf_dir;
    libreoffice_argv[5] = doc_path;

    argv = g_strdupv ((gchar **) libreoffice_argv);
  }

  command = g_strjoinv (" ", (gchar **) argv);
  g_debug ("Executing LibreOffice command: %s", command);

  subprocess = g_subprocess_newv ((const char **) argv, G_SUBPROCESS_FLAGS_NONE, &error);

  if (error) {
    g_warning ("Error while spawning libreoffice: %s", error->message);
    return;
  }

  g_subprocess_wait_async (subprocess, cancellable, libreoffice_done_cb, task);
}

void
sushi_convert_libreoffice (GFile               *file,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  TaskData *data = g_new0 (TaskData, 1);
  data->file = g_object_ref (file);

  g_task_set_task_data (task, data, (GDestroyNotify) task_data_free);
  load_libreoffice (task);
}

/**
 * sushi_convert_libreoffice_finish:
 * @result:
 * @error:
 *
 * Returns: (transfer full):
 */
GFile *
sushi_convert_libreoffice_finish (GAsyncResult *result,
                                  GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/**
 * sushi_running_under_wayland:
 *
 * Returns: Whether we are running under Wayland backend.
 */
gboolean
sushi_running_under_wayland (GdkDisplay *display)
{
#ifdef GDK_WINDOWING_WAYLAND
  return GDK_IS_WAYLAND_DISPLAY (display);
#endif

  return FALSE;
}

struct _SushiDiscoverer {
  GObject parent;

  GstDiscoverer *disco;
  const GstTagList *tag_list;
};

G_DEFINE_TYPE(SushiDiscoverer, sushi_discoverer, G_TYPE_OBJECT)

enum {
  TAGS_CHANGED,
  LAST_SIGNAL
};

static guint disco_signals[LAST_SIGNAL] = { 0, };

static void
discovered_cb (SushiDiscoverer   *self,
               GstDiscovererInfo *info,
               GError            *error)
{
  self->tag_list = gst_discoverer_info_get_tags (info);

  g_signal_emit (self, disco_signals[TAGS_CHANGED], 0);
}

static void
sushi_discoverer_finalize (GObject *object)
{
  SushiDiscoverer *self = SUSHI_DISCOVERER (object);

  if (self->disco)
    gst_discoverer_stop (self->disco);

  g_clear_object (&self->disco);
}

const GstTagList *
sushi_discoverer_get_tag_list (SushiDiscoverer *self)
{
  return self->tag_list;
}

static void
sushi_discoverer_class_init (SushiDiscovererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = sushi_discoverer_finalize;

  disco_signals[TAGS_CHANGED] = g_signal_new ("tags-changed",
                                              G_TYPE_FROM_CLASS (object_class),
                                              G_SIGNAL_RUN_LAST,
                                              0, NULL, NULL, NULL,
                                              G_TYPE_NONE, 0);
}

static void
sushi_discoverer_init (SushiDiscoverer *self)
{
  g_autoptr (GError) error = NULL;

  gst_init_check (NULL, NULL, NULL);
  self->disco = gst_discoverer_new (GST_SECOND * 60, &error);

  if (error)
    {
      g_warning ("Error creating GST discoverer: %s", error->message);
      return;
    }

  g_signal_connect_swapped (self->disco, "discovered", G_CALLBACK (discovered_cb), self);
  gst_discoverer_start (self->disco);
}

SushiDiscoverer *
sushi_discoverer_new (const char *uri)
{
  SushiDiscoverer *self = g_object_new (SUSHI_TYPE_DISCOVERER, NULL);

  if (self->disco)
    gst_discoverer_discover_uri_async (self->disco, uri);

  return self;
}
