/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fingerprint features and matching. The 108x88 sensor is too small for minutiae (NBIS finds
 * 3-9), so we use SIGFM: SIFT keypoints + geometric consistency of matched pairs.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* SIGFM score above which two captures are the same finger. Measured on this sensor
 * (8 enrolled touches): same finger 289 .. 7e5, different finger 0 .. 11. */
#define GV_MATCH_THRESHOLD 100
/* Captures with fewer keypoints carry too little detail to enroll or match. */
#define GV_MIN_KEYPOINTS   25

typedef struct _GvFeatures GvFeatures;

/* Preprocess (background subtraction, normalisation, 2x upscale, CLAHE) and extract features
 * from a capture; both arrays are GV_IMG_PIXELS 12-bit pixels. */
GvFeatures *gv_features_extract (const guint16 *background, const guint16 *frame);
void        gv_features_free (GvFeatures *f);
int         gv_features_keypoints (GvFeatures *f);

GBytes     *gv_features_serialize (GvFeatures *f);
GvFeatures *gv_features_deserialize (const guint8 *data, gsize len);

/* Similarity of a probe against one enrolled capture (higher = more similar). */
int         gv_features_score (GvFeatures *probe, GvFeatures *enrolled);

/* The preprocessed 8-bit image (2x size) that features are extracted from, for diagnostics.
 * Returns a newly allocated buffer of *width x *height bytes. */
guint8     *gv_preprocess (const guint16 *background, const guint16 *frame, int *width, int *height);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GvFeatures, gv_features_free)

G_END_DECLS
