/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Developer tool: genuine/impostor score distribution on local captures.
 *   match-eval DIR_A DIR_B [n_enroll]
 * Each DIR holds bg.bin and fNN.bin (decrypted 14260-byte frames from geneva_driver.py loop).
 */
#include <stdio.h>
#include <stdlib.h>
#include "match.h"
#include "proto.h"

static gboolean
load (const char *path, guint16 *px)
{
  g_autofree char *d = NULL;
  gsize n;

  if (!g_file_get_contents (path, &d, &n, NULL) || n < GV_IMG_PACKED)
    return FALSE;
  gv_image_decode ((const guint8 *) d, px);
  return TRUE;
}

static GPtrArray *
load_dir (const char *dir)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) gv_features_free);
  g_autofree char *bgp = g_build_filename (dir, "bg.bin", NULL);
  guint16 bg[GV_IMG_PIXELS], fr[GV_IMG_PIXELS];

  if (!load (bgp, bg))
    g_error ("cannot read %s", bgp);
  for (int i = 0; i < 100; i++)
    {
      g_autofree char *name = g_strdup_printf ("f%02d.bin", i);
      g_autofree char *p = g_build_filename (dir, name, NULL);

      if (!load (p, fr))
        break;
      g_ptr_array_add (out, gv_features_extract (bg, fr));
    }
  return out;
}

static int
best (GvFeatures *probe, GPtrArray *enrolled, guint n)
{
  int b = 0;

  for (guint i = 0; i < n; i++)
    b = MAX (b, gv_features_score (probe, g_ptr_array_index (enrolled, i)));
  return b;
}

int
main (int argc, char **argv)
{
  g_autoptr(GPtrArray) a = NULL, b = NULL;
  guint n;
  int gmin = G_MAXINT, imax = 0;

  if (argc < 3)
    {
      fprintf (stderr, "usage: %s DIR_A DIR_B [n_enroll]\n", argv[0]);
      return 2;
    }
  a = load_dir (argv[1]);
  b = load_dir (argv[2]);
  n = argc > 3 ? (guint) atoi (argv[3]) : 8;
  printf ("keypoints A[0]=%d B[0]=%d\ngenuine :", gv_features_keypoints (g_ptr_array_index (a, 0)),
          gv_features_keypoints (g_ptr_array_index (b, 0)));
  for (guint i = n; i < a->len; i++)
    {
      int s = best (g_ptr_array_index (a, i), a, n);
      gmin = MIN (gmin, s);
      printf (" %d", s);
    }
  printf ("\nimpostor:");
  for (guint i = 0; i < b->len; i++)
    {
      int s = best (g_ptr_array_index (b, i), a, n);
      imax = MAX (imax, s);
      printf (" %d", s);
    }
  printf ("\ngenuine min %d, impostor max %d (threshold %d)\n", gmin, imax, GV_MATCH_THRESHOLD);
  return 0;
}
