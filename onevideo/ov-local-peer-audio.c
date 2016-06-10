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

#define HAVE_SSE 1

#ifdef HAVE_SSE
  #include <xmmintrin.h>
#endif

#include "ffts.h"
#include "ov-local-peer-audio.h"
#include "ov-local-peer-audio-processing.h"

#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include <gst/base/gstadapter.h>

#define AUDIO_RATE 48000
gsize const frame_buffer_size = AUDIO_RATE * 10 * 2 * 2 / 1000; // 48 khz, 10 ms, 2 channels, 2 byte sample
static gint audioprocessing_init = FALSE;

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
	// GST_WARNING ("asink cb");
  // return GST_PAD_PROBE_OK;
  GstBuffer *buffer, *data_buf;
  static GstAdapter *in_adapter, *out_adapter;
  GstMapInfo map;
	gsize buffer_size;
  buffer = GST_PAD_PROBE_INFO_BUFFER (info);

  if (!in_adapter) {
		GST_WARNING ("Inited adapter asink");
    in_adapter = gst_adapter_new ();
    out_adapter = gst_adapter_new ();
  }
	if (!audioprocessing_init) {
		audioprocessing_init = TRUE;
		GST_WARNING ("Inited audioprocessing unit");
		ov_local_peer_audio_processing_init (AUDIO_RATE, 2);
	}

  buffer = GST_PAD_PROBE_INFO_BUFFER (info);
	buffer_size = gst_buffer_get_size (buffer);

  // GST_DEBUG ("pts %lu dts %lu duration %lu offset %lu offset_delta %lu flags %u",
  //   buffer->pts, buffer->dts, buffer->duration, buffer->offset,
  //   buffer->offset_end - buffer->offset, GST_BUFFER_FLAGS(buffer));

  buffer = gst_buffer_ref (buffer);

  gst_adapter_push (in_adapter, buffer);

  // if we can read out frame_buffer_size bytes, process them
  while (gst_adapter_available (in_adapter) >= frame_buffer_size ) {
		data_buf = gst_adapter_take_buffer (in_adapter, frame_buffer_size);
		data_buf = gst_buffer_make_writable (data_buf);
	  if (gst_buffer_map (data_buf, &map, GST_MAP_WRITE)) {
			ov_local_peer_audio_processing_far_speech_update(map.data, map.size);
    	gst_buffer_unmap (buffer, &map);
      gst_adapter_push (out_adapter, data_buf);
		} else {
			GST_DEBUG ("Failed to map data_buf");
		}
	}

  GST_PAD_PROBE_INFO_DATA (info) = gst_adapter_take_buffer (out_adapter, gst_adapter_available (out_adapter));
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn
ov_asrc_input_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data) {
  GstBuffer *buffer, *data_buf;
  static GstAdapter *in_adapter, *out_adapter;
  GstMapInfo map;

  data_buf = NULL;

  if (!in_adapter) {
		GST_WARNING ("Inited adapter asrc");
    in_adapter = gst_adapter_new ();
    out_adapter = gst_adapter_new ();
  }
	if (!audioprocessing_init) {
		audioprocessing_init = TRUE;
		GST_WARNING ("Inited audioprocessing unit");
		ov_local_peer_audio_processing_init (AUDIO_RATE, 2);
	}

  buffer = GST_PAD_PROBE_INFO_BUFFER (info);

  // GST_DEBUG ("pts %lu dts %lu duration %lu offset %lu offset_delta %lu flags %u",
  //   buffer->pts, buffer->dts, buffer->duration, buffer->offset,
  //   buffer->offset_end - buffer->offset, GST_BUFFER_FLAGS(buffer));

  gst_adapter_push (in_adapter, buffer);

  // if we can read out frame_buffer_size bytes, process them
  while (gst_adapter_available (in_adapter) >= frame_buffer_size ) {
		data_buf = gst_adapter_take_buffer (in_adapter, frame_buffer_size);
		data_buf = gst_buffer_make_writable (data_buf);
	  if (gst_buffer_map (data_buf, &map, GST_MAP_WRITE)) {
			ov_local_peer_audio_processing_near_speech_update(map.data, map.size);
    	gst_buffer_unmap (buffer, &map);
      gst_adapter_push (out_adapter, data_buf);
		} else {
			GST_DEBUG ("Failed to map data_buf");
		}
	}

  GST_PAD_PROBE_INFO_DATA (info) = gst_adapter_take_buffer (out_adapter, gst_adapter_available (out_adapter));
  return GST_PAD_PROBE_OK;
}
