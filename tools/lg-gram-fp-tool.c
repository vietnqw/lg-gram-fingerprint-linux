/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * lg-gram-fp-tool: setup and diagnostics for the Goodix GXFP5A8B ("Geneva") fingerprint sensor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "error.h"
#include "hw.h"
#include "provision.h"
#include "sensor.h"

static char *opt_config = NULL;
static char *opt_out = NULL;
static gboolean opt_yes = FALSE;
static gboolean opt_force = FALSE;
static char *opt_driver = NULL;

static GvSensor *
open_sensor (gboolean need_config, GError **error)
{
  g_autofree char *spidev = gv_find_spidev ();
  guint8 cfg[GV_CFG_SIZE] = { 0 };

  if (!spidev)
    return gv_error_null (error, GV_ERROR_IO,
                          "no spidev node for %s; is the udev rule installed (or reboot after install)?",
                          GV_ACPI_ID);
  if (need_config && !gv_config_load (opt_config ? opt_config : GV_CONFIG_PATH, cfg, error))
    return NULL;
  return gv_sensor_new (spidev, GV_GPIOCHIP, GV_RESET_LINE, cfg, error);
}

static int
cmd_status (int argc, char **argv, GError **error)
{
  g_autoptr(GvSensor) s = open_sensor (FALSE, error);
  g_autofree char *fw = NULL;
  gboolean psk_ok = FALSE;
  guint8 cfg[GV_CFG_SIZE];
  g_autoptr(GError) cfg_err = NULL;

  if (!s || !gv_sensor_status (s, TRUE, &fw, &psk_ok, NULL, error))
    return 1;
  gboolean app = g_str_has_prefix (fw, "GF3268");

  printf ("firmware:       %s%s\n", fw, app ? "" : "  (NOT running the application - run `sudo lg-gram-fp-tool setup`)");
  printf ("paired (PSK):   %s\n", psk_ok ? "yes" : "NO - run `sudo lg-gram-fp-tool setup`");
  printf ("sensor config:  %s\n", gv_config_load (opt_config ? opt_config : GV_CONFIG_PATH, cfg, &cfg_err)
          ? "ok" : cfg_err->message);
  return app && psk_ok && !cfg_err ? 0 : 1;
}

static void
write_pgm (const char *path, const guint16 *bg, const guint16 *frame)
{
  double d[GV_IMG_PIXELS], lo = 1e9, hi = -1e9;
  g_autoptr(GString) out = g_string_new (NULL);

  for (gsize i = 0; i < GV_IMG_PIXELS; i++)
    {
      d[i] = (double) bg[i] - frame[i];
      lo = MIN (lo, d[i]);
      hi = MAX (hi, d[i]);
    }
  g_string_printf (out, "P5\n%d %d\n255\n", GV_IMG_WIDTH, GV_IMG_HEIGHT);
  for (gsize i = 0; i < GV_IMG_PIXELS; i++)
    g_string_append_c (out, (char) (guint8) ((d[i] - lo) * 255.0 / MAX (hi - lo, 1.0)));
  g_file_set_contents (path, out->str, out->len, NULL);
}

static int
cmd_capture (int argc, char **argv, GError **error)
{
  const char *path = argc > 1 ? argv[1] : "fingerprint.pgm";
  g_autoptr(GvSensor) s = open_sensor (TRUE, error);
  guint16 frame[GV_IMG_PIXELS], bg[GV_IMG_PIXELS];
  GvCaptureResult res;

  if (!s)
    return 1;
  for (;;)
    {
      printf ("Touch the sensor...\n");
      fflush (stdout);
      if (!gv_sensor_capture (s, frame, bg, &res, NULL, error))
        return 1;
      if (res == GV_CAPTURE_OK)
        break;
      printf ("Poor contact, lift your finger and try again.\n");
      if (!gv_sensor_wait_lift (s, NULL, error))
        return 1;
    }
  write_pgm (path, bg, frame);
  printf ("Captured (signal %.0f) -> %s\n", gv_signal (bg, frame), path);
  return 0;
}

static int
cmd_extract (int argc, char **argv, GError **error)
{
  const char *out = opt_out ? opt_out : GV_DATA_DIR;

  if (argc < 2)
    {
      fprintf (stderr, "usage: lg-gram-fp-tool extract DRIVER\n"
                       "DRIVER: Microsoft Update .cab, LG's GD_FP_W106_15_3.1.100.320.zip, or Wbdi.dll\n"
                       "(`lg-gram-fp-tool setup` downloads it for you)\n");
      return 2;
    }
  if (!gv_extract (argv[1], out, error))
    return 1;
  printf ("Extracted sensor configuration and firmware to %s\n", out);
  return 0;
}

static void
progress (const char *step, double fraction, gpointer user_data)
{
  printf ("\r  [%3.0f%%] %-40s", fraction * 100, step);
  if (fraction >= 1.0)
    printf ("\n");
  fflush (stdout);
}

/* Yes/no question; --yes answers yes, and so does a non-interactive run only if --yes. */
static gboolean
ask (const char *question, gboolean default_yes)
{
  char answer[16] = "";

  if (opt_yes)
    return TRUE;
  printf ("%s %s ", question, default_yes ? "[Y/n]" : "[y/N]");
  fflush (stdout);
  if (!fgets (answer, sizeof answer, stdin))
    return FALSE;
  g_strstrip (answer);
  if (!*answer)
    return default_yes;
  return answer[0] == 'y' || answer[0] == 'Y';
}

static gboolean
data_present (void)
{
  return g_file_test (GV_CONFIG_PATH, G_FILE_TEST_EXISTS) &&
         g_file_test (GV_FIRMWARE_PATH, G_FILE_TEST_EXISTS);
}

/* Download (or use --driver) and extract the sensor configuration + firmware. */
static gboolean
ensure_data (GError **error)
{
  g_autofree char *cab = NULL;
  gboolean ok;

  if (opt_driver)
    {
      if (!gv_extract (opt_driver, GV_DATA_DIR, error))
        return FALSE;
      printf ("Sensor configuration extracted from %s.\n", opt_driver);
      return TRUE;
    }
  printf ("The sensor needs its configuration from Goodix's Windows driver (the one LG ships).\n"
          "It will be downloaded from Microsoft Update (1.5 MB) and only the sensor settings and\n"
          "firmware are kept, in %s. Nothing is uploaded.\n", GV_DATA_DIR);
  if (!ask ("Download it now?", TRUE))
    {
      g_set_error (error, GV_ERROR, GV_ERROR_NO_CONFIG,
                   "not downloaded; you can instead run `sudo lg-gram-fp-tool setup --driver "
                   "GD_FP_W106_15_3.1.100.320.zip` with LG's driver from LG's support site");
      return FALSE;
    }
  printf ("Downloading...\n");
  cab = gv_download_driver (error);
  if (!cab)
    return FALSE;
  ok = gv_extract (cab, GV_DATA_DIR, error);
  g_unlink (cab);
  {
    g_autofree char *dir = g_path_get_dirname (cab);
    g_rmdir (dir);
  }
  if (ok)
    printf ("Sensor configuration installed.\n");
  return ok;
}

/* Pair the sensor with Linux if needed. Returns FALSE on error or if the user declined. */
static gboolean
ensure_paired (GError **error)
{
  g_autoptr(GvSensor) s = open_sensor (FALSE, error);
  g_autofree char *fw = NULL;
  g_autofree char *image = NULL;
  gsize len = 0;
  gboolean psk_ok = FALSE, ok;
  sigset_t block, old;
  g_autoptr(GSubprocess) inhibit = NULL;

  if (!s || !gv_sensor_status (s, TRUE, &fw, &psk_ok, NULL, error))
    return FALSE;
  if (g_str_has_prefix (fw, "GF3268") && psk_ok && !opt_force)
    {
      printf ("The sensor is paired with Linux.\n");
      return TRUE;
    }
  if (!g_file_get_contents (GV_FIRMWARE_PATH, &image, &len, NULL) || len != GV_FIRMWARE_SIZE)
    {
      g_set_error (error, GV_ERROR, GV_ERROR_NO_FIRMWARE, "%s missing; run `sudo lg-gram-fp-tool setup`",
                   GV_FIRMWARE_PATH);
      return FALSE;
    }

  printf ("\nThe sensor is still paired with Windows (each OS uses its own key for the sensor).\n"
          "Pairing it with Linux rewrites its key and reflashes its own firmware; it takes about\n"
          "30 seconds and is safe to run again if interrupted. If you later use the fingerprint in\n"
          "Windows, Windows takes it back automatically; just run `sudo lg-gram-fp-tool setup` again.\n");
  if (!ask ("Pair the sensor with Linux now?", TRUE))
    return gv_error_false (error, GV_ERROR_NOT_PROVISIONED, "not paired; run `sudo lg-gram-fp-tool setup` later");

  /* Do not let Ctrl-C, a closed terminal or suspend interrupt the flash. */
  inhibit = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                              "systemd-inhibit", "--what=sleep:shutdown:idle", "--who=lg-gram-fp-tool",
                              "--why=Pairing the fingerprint sensor", "sleep", "600", NULL);
  sigemptyset (&block);
  sigaddset (&block, SIGINT);
  sigaddset (&block, SIGTERM);
  sigaddset (&block, SIGHUP);
  sigprocmask (SIG_BLOCK, &block, &old);
  ok = gv_provision (s, (const guint8 *) image, len, progress, NULL, error);
  if (inhibit)
    g_subprocess_force_exit (inhibit);
  sigprocmask (SIG_SETMASK, &old, NULL);
  if (!ok)
    printf ("\n");
  else
    printf ("Sensor paired with Linux.\n");
  return ok;
}

static int
cmd_provision (int argc, char **argv, GError **error)
{
  return ensure_paired (error) ? 0 : 1;
}

static gboolean
pam_enabled (void)
{
  g_autofree char *auth = NULL;

  return g_file_get_contents ("/etc/pam.d/common-auth", &auth, NULL, NULL) && strstr (auth, "pam_fprintd.so");
}

static int
cmd_setup (int argc, char **argv, GError **error)
{
  if (!data_present () || opt_driver)
    {
      if (!ensure_data (error))
        return 1;
      /* let a running fprintd pick up the sensor now that it is usable */
      g_spawn_command_line_sync ("systemctl try-restart fprintd.service", NULL, NULL, NULL, NULL);
    }
  if (!ensure_paired (error))
    return 1;

  if (!pam_enabled () && g_file_test ("/usr/sbin/pam-auth-update", G_FILE_TEST_IS_EXECUTABLE) &&
      ask ("\nUse your fingerprint for sudo and login (password still works)?", TRUE))
    {
      if (g_spawn_command_line_sync ("pam-auth-update --enable lg-gram-fingerprint", NULL, NULL, NULL, NULL))
        printf ("Fingerprint authentication enabled.\n");
    }

  printf ("\nAll set. Enroll a finger in Settings > Users > Fingerprint Login, or run `fprintd-enroll`.\n");
  return 0;
}

static const struct {
  const char *name, *args, *help;
  int (*fn)(int argc, char **argv, GError **error);
} commands[] = {
  { "setup", "", "set everything up (download config, pair, enable)", cmd_setup },
  { "status", "", "show firmware, pairing and configuration state", cmd_status },
  { "capture", "[FILE.pgm]", "capture one fingerprint image", cmd_capture },
  { "extract", "DRIVER", "extract config + firmware from a Goodix driver package", cmd_extract },
  { "provision", "", "pair the sensor with Linux (reflashes it)", cmd_provision },
};

int
main (int argc, char **argv)
{
  g_autoptr(GOptionContext) ctx = g_option_context_new ("COMMAND [ARGS] - Goodix GXFP5A8B sensor tool");
  g_autoptr(GString) summary = g_string_new ("Commands:");
  g_autoptr(GError) error = NULL;
  GOptionEntry entries[] = {
    { "config", 'c', 0, G_OPTION_ARG_FILENAME, &opt_config, "Sensor configuration file", "FILE" },
    { "driver", 'd', 0, G_OPTION_ARG_FILENAME, &opt_driver, "setup: use this driver package instead of downloading", "FILE" },
    { "out", 'o', 0, G_OPTION_ARG_FILENAME, &opt_out, "Output directory for extract", "DIR" },
    { "yes", 'y', 0, G_OPTION_ARG_NONE, &opt_yes, "Answer yes to all questions", NULL },
    { "force", 'f', 0, G_OPTION_ARG_NONE, &opt_force, "Provision even if already paired", NULL },
    { NULL }
  };

  for (gsize i = 0; i < G_N_ELEMENTS (commands); i++)
    g_string_append_printf (summary, "\n  %-10s %-12s %s", commands[i].name, commands[i].args, commands[i].help);
  g_option_context_set_summary (ctx, summary->str);
  g_option_context_add_main_entries (ctx, entries, NULL);
  if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
      fprintf (stderr, "%s\n", error->message);
      return 2;
    }
  if (argc < 2)
    {
      g_autofree char *help = g_option_context_get_help (ctx, TRUE, NULL);
      fputs (help, stderr);
      return 2;
    }
  for (gsize i = 0; i < G_N_ELEMENTS (commands); i++)
    if (g_str_equal (argv[1], commands[i].name))
      {
        int rc = commands[i].fn (argc - 1, argv + 1, &error);

        if (error)
          fprintf (stderr, "error: %s\n", error->message);
        return rc;
      }
  fprintf (stderr, "unknown command '%s' (see --help)\n", argv[1]);
  return 2;
}
