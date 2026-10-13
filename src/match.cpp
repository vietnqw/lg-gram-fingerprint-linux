// SPDX-License-Identifier: LGPL-2.1-or-later
#include "match.h"
#include "proto.h"
#include "sigfm/sigfm.hpp"
#include "sigfm/img-info.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <opencv2/imgproc.hpp>

struct _GvFeatures
{
  SigfmImgInfo *info;
};

/* Percentile with linear interpolation (numpy's default), on a copy. */
static float
percentile (std::vector<float> v, double q)
{
  double pos = q / 100.0 * (v.size () - 1);
  size_t i = (size_t) pos;
  double t = pos - i;

  std::nth_element (v.begin (), v.begin () + i, v.end ());
  float a = v[i];
  if (t == 0.0 || i + 1 >= v.size ())
    return a;
  float b = *std::min_element (v.begin () + i + 1, v.end ());
  return (float) (a + t * (b - a));
}

static cv::Mat
preprocess (const guint16 *bg, const guint16 *frame)
{
  // A finger lowers the signal: bg - frame is bright where ridges touch.
  std::vector<float> d (GV_IMG_PIXELS);
  cv::Mat g (GV_IMG_HEIGHT, GV_IMG_WIDTH, CV_8U), up, out;

  for (int i = 0; i < GV_IMG_PIXELS; i++)
    d[i] = (float) bg[i] - (float) frame[i];

  // Robust contrast stretch between the 1st and 99th percentile (truncating, as numpy does).
  float lo = percentile (d, 1), hi = percentile (d, 99);
  float scale = 255.0f / std::max (hi - lo, 1.0f);
  for (int i = 0; i < GV_IMG_PIXELS; i++)
    g.data[i] = (guint8) std::clamp ((d[i] - lo) * scale, 0.0f, 255.0f);

  cv::resize (g, up, cv::Size (2 * GV_IMG_WIDTH, 2 * GV_IMG_HEIGHT), 0, 0, cv::INTER_CUBIC);
  cv::createCLAHE (2.0, cv::Size (4, 4))->apply (up, out);
  return out;
}

guint8 *
gv_preprocess (const guint16 *bg, const guint16 *frame, int *width, int *height)
{
  cv::Mat m = preprocess (bg, frame);
  guint8 *buf = (guint8 *) g_malloc (m.total ());

  std::memcpy (buf, m.data, m.total ());
  *width = m.cols;
  *height = m.rows;
  return buf;
}

GvFeatures *
gv_features_extract (const guint16 *bg, const guint16 *frame)
{
  cv::Mat m = preprocess (bg, frame);
  SigfmImgInfo *info = sigfm_extract (m.data, m.cols, m.rows);

  if (!info)
    return nullptr;
  return new GvFeatures{ info };
}

void
gv_features_free (GvFeatures *f)
{
  if (!f)
    return;
  sigfm_free_info (f->info);
  delete f;
}

int
gv_features_keypoints (GvFeatures *f)
{
  return sigfm_keypoints_count (f->info);
}

/* Compact template format (SIGFM only uses keypoint positions and descriptors):
 *   "GVF1" | u16 count | count x (f32 x, f32 y) | count x 128 u8 descriptor
 * OpenCV's SIFT descriptors are integers 0..255 stored as floats, so u8 is lossless and
 * ~4x smaller than SIGFM's own serialisation. */
#define FEAT_MAGIC "GVF1"
#define DESC_LEN   128

GBytes *
gv_features_serialize (GvFeatures *f)
{
  const auto &kp = f->info->keypoints;
  const cv::Mat &desc = f->info->descriptors;
  guint16 count = (guint16) MIN (kp.size (), (size_t) G_MAXUINT16);
  GByteArray *out = g_byte_array_sized_new (6 + count * (8 + DESC_LEN));

  g_byte_array_append (out, (const guint8 *) FEAT_MAGIC, 4);
  g_byte_array_append (out, (const guint8 *) &count, 2);
  for (guint16 i = 0; i < count; i++)
    {
      float xy[2] = { kp[i].pt.x, kp[i].pt.y };
      g_byte_array_append (out, (const guint8 *) xy, sizeof xy);
    }
  for (guint16 i = 0; i < count; i++)
    for (int j = 0; j < DESC_LEN; j++)
      {
        guint8 v = cv::saturate_cast<guint8> (desc.at<float> (i, j));
        g_byte_array_append (out, &v, 1);
      }
  return g_byte_array_free_to_bytes (out);
}

GvFeatures *
gv_features_deserialize (const guint8 *data, gsize len)
{
  guint16 count;
  SigfmImgInfo *info;

  if (len < 6 || memcmp (data, FEAT_MAGIC, 4) != 0)
    return nullptr;
  memcpy (&count, data + 4, 2);
  if (len != 6 + (gsize) count * (8 + DESC_LEN))
    return nullptr;

  info = new SigfmImgInfo;
  info->descriptors.create (count, DESC_LEN, CV_32F);
  for (guint16 i = 0; i < count; i++)
    {
      float xy[2];
      memcpy (xy, data + 6 + 8 * i, sizeof xy);
      info->keypoints.emplace_back (cv::Point2f (xy[0], xy[1]), 1.0f);
    }
  const guint8 *d = data + 6 + 8 * count;
  for (guint16 i = 0; i < count; i++)
    for (int j = 0; j < DESC_LEN; j++)
      info->descriptors.at<float> (i, j) = d[i * DESC_LEN + j];
  return new GvFeatures{ info };
}

int
gv_features_score (GvFeatures *probe, GvFeatures *enrolled)
{
  return std::max (0, sigfm_match_score (probe->info, enrolled->info));
}
