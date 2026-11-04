#pragma once
/* Shared bitmap decoder for user programs (M8).
 *
 * The browser fetches inline <img> bitmaps and the picture viewer reads files
 * from /pictures; both need the same PNG/BMP/PPM support, so the codec lives
 * here instead of in one program.
 *
 * Supported inputs:
 *   PNG  8-bit, non-interlaced, colour types 0/2/3/4/6
 *   BMP  uncompressed 24-bit and 32-bit
 *   PPM  P6 (binary) and P5 (binary grey), plus P3 (ASCII)
 *
 * Output pixels are 0x00RRGGBB in row-major order. The caller owns every
 * buffer: `out` must hold IMGDEC_MAX_PX pixels, `aux` receives the
 * concatenated PNG IDAT stream and `rows` the inflated scanlines. BMP and PPM
 * ignore the two scratch buffers.
 */

#include <mcube/types.h>

/* Largest image accepted by img_decode(). Keeps every buffer size predictable
 * on a machine with no swap. */
#define IMGDEC_MAX_PX (1280 * 960)

/* Failure reasons reported by img_last_error() after img_decode() returns -1. */
#define IMGDEC_ERR_NONE        0
#define IMGDEC_ERR_UNSUPPORTED 1   /* format/variant the codecs do not handle */
#define IMGDEC_ERR_TOO_LARGE   2   /* pixel count above IMGDEC_MAX_PX */
#define IMGDEC_ERR_CORRUPT     3   /* bad header, truncated or bad stream */

/* Returns 0 on success and fills out_w, out_h and the first w * h pixels of
 * `out`. Any unsupported or corrupt input returns -1. */
int img_decode(const u8 *data, int len, u32 *out, int *out_w, int *out_h,
               u8 *aux, int aux_cap, u8 *rows, int rows_cap);

/* Reason for the most recent img_decode() call; lets callers show a precise
 * message ("too large" vs "unsupported") instead of one generic error. */
int img_last_error(void);

/* True when the file name has a .png/.bmp/.ppm extension (case-insensitive).
 * This only filters directory listings; img_decode still sniffs the data. */
int img_name_supported(const char *name);
