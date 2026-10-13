/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Matcher sanity checks on synthetic ridge patterns (no biometric data in the repo).
 */
#include <math.h>
#include "match.h"
#include "proto.h"

/* A whorl-like ridge pattern; (dx, dy) shifts the finger, (lobes, period) change its shape. */
static void
synth (guint16 *bg, guint16 *frame, double lobes, double period,
       double dx, double dy, guint seed)
{
  GRand *r = g_rand_new_with_seed (seed);

  for (int y = 0; y < GV_IMG_HEIGHT; y++)
    for (int x = 0; x < GV_IMG_WIDTH; x++)
      {
        double u = x - 40 - dx, v = y - 40 - dy;
        double rad = sqrt (u * u + 0.6 * v * v) + 3.0 * sin (atan2 (v, u) * lobes);
        double ridge = 0.5 + 0.5 * cos (rad * 2.0 * G_PI / period);
        int i = y * GV_IMG_WIDTH + x;

        bg[i] = 2400 + g_rand_int_range (r, -4, 5);
        frame[i] = bg[i] - (guint16) (300 + 600 * ridge) + g_rand_int_range (r, -4, 5);
      }
  g_rand_free (r);
}

static void
test_match (void)
{
  guint16 bg[GV_IMG_PIXELS], f1[GV_IMG_PIXELS], f2[GV_IMG_PIXELS], f3[GV_IMG_PIXELS];
  g_autoptr(GvFeatures) a = NULL, b = NULL, c = NULL, a2 = NULL;
  g_autoptr(GBytes) blob = NULL;
  int same, other;

  synth (bg, f1, 3, 8, 0, 0, 1);
  a = gv_features_extract (bg, f1);
  synth (bg, f2, 3, 8, 5, -3, 2);                   /* same "finger", moved */
  b = gv_features_extract (bg, f2);
  synth (bg, f3, 5, 6.5, 0, 0, 3);                  /* different "finger" */
  c = gv_features_extract (bg, f3);

  g_assert_cmpint (gv_features_keypoints (a), >=, GV_MIN_KEYPOINTS);
  same = gv_features_score (b, a);
  other = gv_features_score (c, a);
  g_test_message ("same %d other %d", same, other);
  g_assert_cmpint (same, >=, GV_MATCH_THRESHOLD);
  g_assert_cmpint (other, <, GV_MATCH_THRESHOLD);

  /* serialisation round trip keeps the score */
  blob = gv_features_serialize (a);
  a2 = gv_features_deserialize (g_bytes_get_data (blob, NULL), g_bytes_get_size (blob));
  g_assert_nonnull (a2);
  g_assert_cmpint (gv_features_score (b, a2), ==, same);
  g_assert_null (gv_features_deserialize ((const guint8 *) "junk", 4));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/match/synthetic", test_match);
  return g_test_run ();
}
