/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * geneva-fp-tool: setup and diagnostics for the Goodix GXFP5A8B ("Geneva") fingerprint sensor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>

#include "error.h"
#include "hw.h"
#include "sensor.h"

static char *opt_config = NULL;

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

  if (!s || !gv_sensor_status (s, &fw, &psk_ok, NULL, error))
    return 1;
  printf ("firmware:       %s\n", fw);
  printf ("paired (PSK):   %s\n", psk_ok ? "yes" : "NO - run `sudo geneva-fp-tool provision`");
  printf ("sensor config:  %s\n", gv_config_load (opt_config ? opt_config : GV_CONFIG_PATH, cfg, &cfg_err)
          ? "ok" : cfg_err->message);
  return psk_ok && !cfg_err ? 0 : 1;
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

static const struct {
  const char *name, *args, *help;
  int (*fn)(int argc, char **argv, GError **error);
} commands[] = {
  { "status", "", "show firmware, pairing and configuration state", cmd_status },
  { "capture", "[FILE.pgm]", "capture one fingerprint image", cmd_capture },
};

int
main (int argc, char **argv)
{
  g_autoptr(GOptionContext) ctx = g_option_context_new ("COMMAND [ARGS] - Goodix GXFP5A8B sensor tool");
  g_autoptr(GString) summary = g_string_new ("Commands:");
  g_autoptr(GError) error = NULL;
  GOptionEntry entries[] = {
    { "config", 'c', 0, G_OPTION_ARG_FILENAME, &opt_config, "Sensor configuration file", "FILE" },
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
