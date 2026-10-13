/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Developer harness: drive the TOD driver through libfprint without fprintd.
 *   FP_TOD_DRIVERS_DIR=build fp-harness enroll FILE
 *   FP_TOD_DRIVERS_DIR=build fp-harness verify FILE [N]
 *   FP_TOD_DRIVERS_DIR=build fp-harness identify FILE...
 */
#include <stdio.h>
#include <stdlib.h>
#include <fprint.h>

static void
enroll_progress (FpDevice *dev, gint stage, FpPrint *print, gpointer data, GError *error)
{
  if (error)
    printf ("  retry: %s\n", error->message);
  else
    printf ("  stage %d/%d - lift and touch again\n", stage, fp_device_get_nr_enroll_stages (dev));
  fflush (stdout);
}

static FpPrint *
load_print (const char *path)
{
  g_autofree guchar *d = NULL;
  gsize n;
  g_autoptr(GError) e = NULL;
  FpPrint *p;

  if (!g_file_get_contents (path, (char **) &d, &n, &e))
    g_error ("%s", e->message);
  p = fp_print_deserialize (d, n, &e);
  if (!p)
    g_error ("%s", e->message);
  return p;
}

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) ctx = fp_context_new ();
  g_autoptr(GError) error = NULL;
  GPtrArray *devs = fp_context_get_devices (ctx);
  FpDevice *dev;

  if (argc < 3 || !devs->len)
    {
      fprintf (stderr, "usage: %s enroll|verify|identify FILE...  (%u devices)\n", argv[0], devs->len);
      return 2;
    }
  dev = g_ptr_array_index (devs, 0);
  printf ("device: %s (%s)\n", fp_device_get_name (dev), fp_device_get_driver (dev));
  if (!fp_device_open_sync (dev, NULL, &error))
    {
      fprintf (stderr, "open: %s\n", error->message);
      return 1;
    }

  if (g_str_equal (argv[1], "enroll"))
    {
      FpPrint *tmpl = fp_print_new (dev);
      g_autoptr(FpPrint) print = NULL;
      g_autofree guchar *d = NULL;
      gsize n;

      fp_print_set_finger (tmpl, FP_FINGER_RIGHT_INDEX);
      printf ("Touch the sensor %d times...\n", fp_device_get_nr_enroll_stages (dev));
      print = fp_device_enroll_sync (dev, tmpl, NULL, enroll_progress, NULL, &error);
      if (!print || !fp_print_serialize (print, &d, &n, &error))
        {
          fprintf (stderr, "enroll: %s\n", error->message);
          return 1;
        }
      g_file_set_contents (argv[2], (char *) d, n, NULL);
      printf ("enrolled -> %s (%zu bytes)\n", argv[2], n);
    }
  else if (g_str_equal (argv[1], "verify"))
    {
      g_autoptr(FpPrint) print = load_print (argv[2]);
      int rounds = argc > 3 ? atoi (argv[3]) : 1, ok = 0;

      for (int i = 0; i < rounds; i++)
        {
          gboolean match = FALSE;

          printf ("[%d/%d] touch the sensor...\n", i + 1, rounds);
          fflush (stdout);
          g_clear_error (&error);
          if (!fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error))
            printf ("  error: %s\n", error->message);
          else
            printf ("  %s\n", match ? "MATCH" : "no match");
          ok += match;
        }
      printf ("matched %d/%d\n", ok, rounds);
    }
  else if (g_str_equal (argv[1], "identify"))
    {
      g_autoptr(GPtrArray) gallery = g_ptr_array_new_with_free_func (g_object_unref);
      g_autoptr(FpPrint) match = NULL;

      for (int i = 2; i < argc; i++)
        g_ptr_array_add (gallery, load_print (argv[i]));
      printf ("touch the sensor...\n");
      if (!fp_device_identify_sync (dev, gallery, NULL, NULL, NULL, &match, NULL, &error))
        printf ("error: %s\n", error->message);
      else
        for (guint i = 0; i < gallery->len; i++)
          if (match == g_ptr_array_index (gallery, i))
            printf ("identified: %s\n", argv[2 + i]);
      if (!match && !error)
        printf ("no match\n");
    }
  fp_device_close_sync (dev, NULL, NULL);
  return 0;
}
