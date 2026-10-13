/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * lg-gram-fp-tool: setup and diagnostics for the Goodix GXFP5A8B ("Geneva") fingerprint sensor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <gio/gio.h>

#include "error.h"
#include "hw.h"
#include "provision.h"
#include "sensor.h"

static char *opt_config = NULL;
static char *opt_out = NULL;
static gboolean opt_yes = FALSE;
static gboolean opt_force = FALSE;

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

  printf ("firmware:       %s%s\n", fw, app ? "" : "  (NOT running the application - run `sudo lg-gram-fp-tool provision`)");
  printf ("paired (PSK):   %s\n", psk_ok ? "yes" : "NO - run `sudo lg-gram-fp-tool provision`");
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
      fprintf (stderr, "usage: lg-gram-fp-tool extract <LG driver .zip or Wbdi.dll>\n"
                       "Download \"Fingerprint Driver\" 3.1.100.320 for your LG gram from LG's support site.\n");
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

static int
cmd_provision (int argc, char **argv, GError **error)
{
  g_autoptr(GvSensor) s = open_sensor (FALSE, error);
  g_autofree char *fw = NULL;
  g_autofree char *image = NULL;
  gsize len = 0;
  gboolean psk_ok = FALSE;
  char answer[16] = "";

  if (!s || !gv_sensor_status (s, TRUE, &fw, &psk_ok, NULL, error))
    return 1;
  if (g_str_has_prefix (fw, "GF3268") && psk_ok && !opt_force)
    {
      printf ("The sensor is already paired with Linux; nothing to do (--force to redo).\n");
      return 0;
    }
  if (!g_file_get_contents (GV_FIRMWARE_PATH, &image, &len, NULL) || len != GV_FIRMWARE_SIZE)
    {
      g_set_error (error, GV_ERROR, GV_ERROR_NO_FIRMWARE,
                   "%s missing; run `sudo lg-gram-fp-tool extract <LG driver>` first", GV_FIRMWARE_PATH);
      return 1;
    }

  printf ("Current firmware: %s, paired with Linux: %s\n\n", fw, psk_ok ? "yes" : "no");
  printf ("This pairs the fingerprint sensor with Linux. It erases the sensor's application,\n"
          "writes a new pairing key and reflashes the same firmware (about 30 seconds).\n"
          "Do not suspend or power off meanwhile. If it is interrupted, simply run it again.\n\n"
          "Windows Hello fingerprint will stop working until LG's Windows driver re-pairs the\n"
          "sensor (it does that automatically); after that, run this command again in Linux.\n\n");
  if (!opt_yes)
    {
      printf ("Type 'yes' to continue: ");
      fflush (stdout);
      if (!fgets (answer, sizeof answer, stdin) || !g_str_equal (g_strstrip (answer), "yes"))
        {
          printf ("Aborted.\n");
          return 1;
        }
    }
  {
    /* Do not let Ctrl-C, a closed terminal or suspend interrupt the flash. */
    sigset_t block, old;
    g_autoptr(GSubprocess) inhibit = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                       G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                                                       "systemd-inhibit", "--what=sleep:shutdown:idle",
                                                       "--who=lg-gram-fp-tool",
                                                       "--why=Reflashing the fingerprint sensor",
                                                       "sleep", "600", NULL);
    gboolean ok;

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
      {
        printf ("\n");
        return 1;
      }
  }
  printf ("Sensor paired. You can now enroll fingers (e.g. `fprintd-enroll`).\n");
  return 0;
}

static const struct {
  const char *name, *args, *help;
  int (*fn)(int argc, char **argv, GError **error);
} commands[] = {
  { "status", "", "show firmware, pairing and configuration state", cmd_status },
  { "capture", "[FILE.pgm]", "capture one fingerprint image", cmd_capture },
  { "extract", "LG-DRIVER", "extract sensor config + firmware from LG's driver", cmd_extract },
  { "provision", "", "pair the sensor with Linux (one-time, reflashes it)", cmd_provision },
};

int
main (int argc, char **argv)
{
  g_autoptr(GOptionContext) ctx = g_option_context_new ("COMMAND [ARGS] - Goodix GXFP5A8B sensor tool");
  g_autoptr(GString) summary = g_string_new ("Commands:");
  g_autoptr(GError) error = NULL;
  GOptionEntry entries[] = {
    { "config", 'c', 0, G_OPTION_ARG_FILENAME, &opt_config, "Sensor configuration file", "FILE" },
    { "out", 'o', 0, G_OPTION_ARG_FILENAME, &opt_out, "Output directory for extract", "DIR" },
    { "yes", 'y', 0, G_OPTION_ARG_NONE, &opt_yes, "Do not ask for confirmation", NULL },
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
