/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Developer tool: genuine/impostor score distribution on your own captures.
 *   sudo lg-gram-fp-tool collect a 15     # 15 touches of one finger
 *   sudo lg-gram-fp-tool collect b 8      # 8 touches of another finger
 *   match-eval a b [n_enroll]
 * Enrolls the first n_enroll (default 8) captures of A, then scores the rest of A (same
 * finger) and all of B (different finger) against them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "match.h"
#include "proto.h"

static const char PGM_HEADER[] = "P5\n108 88\n4095\n";

/* 16-bit PGM written by `lg-gram-fp-tool collect` */
static gboolean
load (const char *path, guint16 *px)
{
  g_autofree char *d = NULL;
  gsize n, h = strlen (PGM_HEADER);

  if (!g_file_get_contents (path, &d, &n, NULL) || n != h + 2 * GV_IMG_PIXELS ||
      memcmp (d, PGM_HEADER, h) != 0)
    return FALSE;
  for (gsize i = 0; i < GV_IMG_PIXELS; i++)
    px[i] = ((guint8) d[h + 2 * i] << 8) | (guint8) d[h + 2 * i + 1];
  return TRUE;
}

static GPtrArray *
load_dir (const char *dir)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) gv_features_free);
  guint16 bg[GV_IMG_PIXELS], fr[GV_IMG_PIXELS];

  for (int i = 0; i < 100; i++)
    {
      g_autofree char *f = g_strdup_printf ("%s/f%02d.pgm", dir, i);
      g_autofree char *b = g_strdup_printf ("%s/bg%02d.pgm", dir, i);

      if (!load (f, fr) || !load (b, bg))
        break;
      g_ptr_array_add (out, gv_features_extract (bg, fr));
    }
  if (!out->len)
    g_error ("no captures in %s (see `lg-gram-fp-tool collect`)", dir);
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
  n = MIN (argc > 3 ? (guint) atoi (argv[3]) : 8, a->len - 1);
  printf ("genuine :");
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
