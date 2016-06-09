/*  vim: set sts=2 sw=2 et :
 *
 *  Copyright (C) 2015 Centricular Ltd
 *  Author(s): Joe Gorse <jhgorse@gmail.com>,
 *             Nirbheek Chauhan <nirbheek@centricular.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>

#define HAVE_SSE 1

#ifdef HAVE_SSE
	#include <xmmintrin.h>
#endif

#include "ffts.h"
#include "ov-local-peer-audio.h"

typedef float FFT_TYPE_DATA;
typedef double FILTER_TYPE_DATA;

// 480 x 2 channel samples, zero padded to 512
// static uint32_t asink_N = 480;
#define asink_N 512
static FFT_TYPE_DATA __attribute__ ((aligned(32))) asink_input[asink_N*2];
static FFT_TYPE_DATA __attribute__ ((aligned(32))) asink_output[asink_N*2];
// FFT_TYPE_DATA __attribute__ ((aligned(32))) *asink_input = _mm_malloc(2 * asink_N * sizeof(FFT_TYPE_DATA), 32);
// FFT_TYPE_DATA __attribute__ ((aligned(32))) *asink_output = _mm_malloc(2 * asink_N * sizeof(FFT_TYPE_DATA), 32);

/* Bring your own coefficients and data history buffers. */
typedef struct {
  // char name[32];           // Just in case you use these things
  size_t           taps;   // How many taps we have
  FILTER_TYPE_DATA *coef;  // Pointer to array of coefficients, taps long
  FILTER_TYPE_DATA *x;     // History of inputs, taps long
  size_t           ix;     // Iterator
  // bool             is_initialized;
} FIR_FILTER;

/* Give it a new value, get one in return. */
FILTER_TYPE_DATA
fir_filter (FILTER_TYPE_DATA new_value, FIR_FILTER *fir) {
  FILTER_TYPE_DATA sum = 0;
  fir->x[fir->ix] = new_value;
  fir->ix = (fir->ix + 1) % fir->taps;  // Never exceed taps for index

  for (int i=0;i<fir->taps;i++)
    sum += fir->x[(fir->ix + i) % fir->taps] * fir->coef[i];
  return sum;
}

GstPadProbeReturn
ov_asink_input_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data) {
  double rms_power = 0;
  int i = 0;
  char audio_buffer[960];
  int16_t audio_point = 0;

  // asink_ascii_s16le.txt
  // ov_asink_timeline.txt
  static FILE *fd_asink_time;
  static FILE *fd_asink_audio;
  if (!fd_asink_time) {
    fd_asink_time = fopen ("/Users/jhg/gst/master/onevideo/audio_dir/ov_asink_timeline.txt", "w");
		rewind (fd_asink_time);
	}
  if (!fd_asink_audio) {
    fd_asink_audio = fopen ("/Users/jhg/gst/master/onevideo/audio_dir/asink_ascii_s16le.txt", "w");
		rewind (fd_asink_audio);
	}

  /*  */
  GstMapInfo map;
  GstBuffer *buffer;

  buffer = GST_PAD_PROBE_INFO_BUFFER (info);

  // GST_DEBUG ("pts %lu dts %lu duration %lu offset %lu offset_delta %lu flags %u",
  // 	buffer->pts, buffer->dts, buffer->duration, buffer->offset,
	// 	buffer->offset_end - buffer->offset, GST_BUFFER_FLAGS(buffer));

  buffer = gst_buffer_make_writable (buffer);
  if (buffer == NULL)
    return GST_PAD_PROBE_OK;

  /* Mapping a buffer can fail (non-writable) */
  if (gst_buffer_map (buffer, &map, GST_MAP_READ)) {
    // GST_DEBUG("gst_buffer_map size %lu maxsize %lu", map.size, map.maxsize);

    for(i=0;i<map.size/4;i++) {
      audio_point = (short)(map.data[4*i+1] << 8 | map.data[4*i]); // Little endian
      fprintf (fd_asink_audio, "%d\n", audio_point);

      audio_buffer[2*i]   = map.data[4*i]; // Skip two bytes
      audio_buffer[2*i+1] = map.data[4*i+1];
      asink_input[2*i]    = audio_point;

      rms_power += audio_point*audio_point;

    }
    ffts_plan_t *p = ffts_init_1d(asink_N, 1);
    ffts_execute (p, asink_input, asink_output);

    rms_power = sqrt (rms_power/i); // i = N
    // GST_DEBUG ("rms_power %f audio_point %d",
    //             rms_power, audio_point);

    fprintf (fd_asink_time, OV_LP_STRBUF ("ov_asink"));
		fflush (fd_asink_time);
		fflush (fd_asink_audio);

    gst_buffer_unmap (buffer, &map);
  } else {
    GST_DEBUG ("Failed to map buffer.");
  }

  GST_PAD_PROBE_INFO_DATA (info) = buffer;

  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn
ov_asrc_input_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data) {
  //  GST_DEBUG ("ov_asrc_input_cb entered.");
  // gint x, y;
  GstMapInfo map;
  // guint16 *ptr, t;
  GstBuffer *buffer;

  double rms_power = 0;
  int i = 0;
  char audio_buffer[960];
  int16_t audio_point = 0;

  // asrc_ascii_s16le.txt
  // ov_asrc_timeline.txt
  static FILE *fd_asrc_time;
  static FILE *fd_asrc_audio;

  if (!fd_asrc_time) {
		fd_asrc_time = fopen ("/Users/jhg/gst/master/onevideo/audio_dir/ov_asrc_timeline.txt", "w");
		rewind (fd_asrc_time);
	}
  if (!fd_asrc_audio) {
		fd_asrc_audio = fopen ("/Users/jhg/gst/master/onevideo/audio_dir/asrc_ascii_s16le.txt", "w");
		rewind (fd_asrc_audio);
	}

  buffer = GST_PAD_PROBE_INFO_BUFFER (info);

	GST_DEBUG ("pts %lu dts %lu duration %lu offset %lu offset_delta %lu flags %u",
  	buffer->pts, buffer->dts, buffer->duration, buffer->offset,
		buffer->offset_end - buffer->offset, GST_BUFFER_FLAGS(buffer));

  buffer = gst_buffer_make_writable (buffer);
  if (buffer == NULL)
    return GST_PAD_PROBE_OK;

  /* Mapping a buffer can fail (non-writable) */
  if (gst_buffer_map (buffer, &map, GST_MAP_WRITE)) {  // GST_MAP_WRITE ? was GST_MAP_READ
    // GST_DEBUG(" gst_buffer_map size %lu maxsize %lu", map.size, map.maxsize);
    for(i=0;i<map.size/4;i++) {
      audio_point = (short)(map.data[4*i+1] << 8 | map.data[4*i]); // Little endian
      // audio_point = (int16_t)(map.data[4*i] << 8 | map.data[4*i+1]); // Big endian
      fprintf (fd_asrc_audio, "%d\n", audio_point);

      rms_power += audio_point*audio_point;
      audio_buffer[2*i]   = map.data[4*i]; // Skip two bytes
      audio_buffer[2*i+1] = map.data[4*i+1];
    }
    // rms_power = sqrt(rms_power/i); // i = N
    // GST_DEBUG (" rms_power %f audio_point %d",
                // rms_power, audio_point);

    fprintf (fd_asrc_time, OV_LP_STRBUF("ov_asrc"));
		fflush (fd_asrc_time);
		fflush (fd_asrc_audio);

    gst_buffer_unmap (buffer, &map);
  } else {
    GST_DEBUG("Failed to map buffer.");
  }

  GST_PAD_PROBE_INFO_DATA (info) = buffer;
  return GST_PAD_PROBE_OK;
}
