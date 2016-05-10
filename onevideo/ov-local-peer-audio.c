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

#define HAVE_SSE 1

#ifdef HAVE_SSE
	#include <xmmintrin.h>
#endif

#include "ffts.h"
#include "ov-local-peer-audio.h"

#include "zhelpers.h"

void *ov_zmq_context = NULL;
void *ov_zmq_publisher = NULL;

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
FILTER_TYPE_DATA fir_filter (FILTER_TYPE_DATA new_value, FIR_FILTER *fir) {
  FILTER_TYPE_DATA sum = 0;
  fir->x[fir->ix] = new_value;
  fir->ix = (fir->ix + 1) % fir->taps;  // Never exceed taps for index

  for (int i=0;i<fir->taps;i++)
    sum += fir->x[(fir->ix + i) % fir->taps] * fir->coef[i];
  return sum;
}

static void
ov_zmq_init (void) {
  GST_DEBUG("ZMQ Init");
  ov_zmq_context = zmq_ctx_new ();
  ov_zmq_publisher = zmq_socket (ov_zmq_context, ZMQ_PUB);
  int rc = zmq_bind (ov_zmq_publisher, "tcp://*:5566");
  assert (rc == 0);
}

GstPadProbeReturn
ov_asink_input_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data) {
  static GSocketAddress *sock_addr = NULL;
  static GSocket *socket = NULL;
  GError *error = NULL;
  gssize written;

  double rms_power = 0;
  int i = 0;
  char audio_buffer[960];
  static char str_buffer[128];
  int16_t audio_point = 0;

  // Filter init
  // This is the wrong filter. 8 kHz 12 order FIR
  static FILTER_TYPE_DATA fir_filter_coef[] = {
    0.0784916072926120,0.109898438593054,0.131207239031524,0.238748168945313,0.131207239031524,0.109898438593054,0.0784916072926120,0.0422574646143064,0.00699944268384739,-0.0220899985057823,-0.0220899985057823,0.00699944268384740,0.0422574646143064
  }; // 13
  static FILTER_TYPE_DATA fir_history[] = { 0,0,0, 0,0,0,0,0, 0,0,0,0,0 };
  static FIR_FILTER a = {sizeof(fir_filter_coef)/sizeof(FILTER_TYPE_DATA),
                        fir_filter_coef, fir_history, 0 };

  if (!ov_zmq_context) {
    ov_zmq_init();
  }

  // if (a.taps < 2) {
  //   a.taps = sizeof(fir_filter_coef)/sizeof(FILTER_TYPE_DATA);
  //   a.x    = fir_history;
  //   a.coef = fir_filter_coef;
  //   a.ix = 0;
  // }

// Recording this stream to 1 column of text
// nc -ulk 1234 | pv -cN binary | od -t d2 -An -v | tr -su ' ' '\n' | pv -cN recording > live.txt
  if (!sock_addr) {
    sock_addr = g_inet_socket_address_new_from_string ("127.0.0.1", 1234);
  }
  if (!socket) {
  socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
      G_SOCKET_PROTOCOL_UDP, &error);
      if (!socket) {
        GST_ERROR ("Unable to create new socket: %s", error->message);
        goto err;
      }
  }

  /*  */
  GstMapInfo map;
  GstBuffer *buffer;

  buffer = GST_PAD_PROBE_INFO_BUFFER (info);

  // GST_DEBUG ("pts %lu dts %lu duration %lu offset %lu offset_end %lu flags %u",
  // buffer->pts, buffer->dts, buffer->duration, buffer->offset, buffer->offset_end, GST_BUFFER_FLAGS(buffer));

  buffer = gst_buffer_make_writable (buffer);
  if (buffer == NULL)
    return GST_PAD_PROBE_OK;

  /* Mapping a buffer can fail (non-writable) */
  if (gst_buffer_map (buffer, &map, GST_MAP_READ)) {
    // GST_DEBUG("gst_buffer_map size %lu maxsize %lu", map.size, map.maxsize);
    for(i=0;i<map.size/4;i++) {
      audio_point = (short)(map.data[4*i+1] << 8 | map.data[4*i]); // Little endian
      // printf("map.data %02x %02x ", map.data[4*i+1], map.data[4*i]);
      // Send unfiltered data
      audio_buffer[2*i]   = map.data[4*i]; // Skip two bytes
      audio_buffer[2*i+1] = map.data[4*i+1];

      asink_input[2*i]    = audio_point;

      audio_point = fir_filter (audio_point, &a);
      map.data[4*i+1] = (char)(audio_point >> 8);
      map.data[4*i]   = (char)(audio_point);
      map.data[4*i+3] = map.data[4*i+1]; // Both channels need it
      map.data[4*i+2] = map.data[4*i];

      // map.data[4*i+1] = 0; // Complete attenuation
      // map.data[4*i]   = 0;
      // map.data[4*i+2] = 0; // Complete attenuation
      // map.data[4*i+3] = 0;

      // printf("  map.data %02x %02x ", map.data[4*i+1], map.data[4*i]);

      rms_power += audio_point*audio_point;

      // printf("%02x", map.data[i]);
      // if (i % 4 == 0)
      //   printf(" ");
      // if (i % 8 == 0)
      //   printf(" ");
      // if (i % 16 == 0)
      //   printf("\n");
    }
    ffts_plan_t *p = ffts_init_1d(asink_N, 1);
    ffts_execute(p, asink_input, asink_output);

    rms_power = sqrt(rms_power/i); // i = N
    GST_DEBUG ("rms_power %f audio_point %d",
                rms_power, audio_point);

    written = g_socket_send_to (socket, sock_addr, (const gchar *)audio_buffer,
                                sizeof(audio_buffer), NULL, &error);
    if (written < sizeof(audio_buffer)) {
      GST_ERROR ("Unable to g_socket_send_to: %s \nwritten: %ld",
      error->message, written);
    }

    OV_ZMQ_STRBUF("ov_asink");
    // sprintf (str_buffer, "ov_asink %" OV_GST_TIME_FORMAT " duration %lu offset %lu offset_end %lu",
    //   OV_GST_TIME_ARGS(gst_util_get_timestamp()), buffer->duration, buffer->offset, buffer->offset_end);
    s_send (ov_zmq_publisher, str_buffer);

    gst_buffer_unmap (buffer, &map);
  } else {
    GST_DEBUG ("Failed to map buffer.");
  }

  GST_PAD_PROBE_INFO_DATA (info) = buffer;
err:
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
  static char str_buffer[128];
  int16_t audio_point = 0;

  buffer = GST_PAD_PROBE_INFO_BUFFER (info);

  if (!ov_zmq_context) {
    ov_zmq_init();
  }
  ///
  // GST_DEBUG("pts %lu dts %lu duration %lu offset %lu offset_end %lu flags %u",
  // buffer->pts, buffer->dts, buffer->duration, buffer->offset, buffer->offset_end, GST_BUFFER_FLAGS(buffer));

  //buffer = gst_buffer_make_writable (buffer);
  // if (buffer == NULL)
  //   return GST_PAD_PROBE_OK;

  /* Mapping a buffer can fail (non-writable) */
  if (gst_buffer_map (buffer, &map, GST_MAP_READ)) {
    // GST_DEBUG(" gst_buffer_map size %lu maxsize %lu", map.size, map.maxsize);
    for(i=0;i<map.size/4;i++) {
      audio_point = (short)(map.data[4*i+1] << 8 | map.data[4*i]); // Little endian
      // audio_point = (int16_t)(map.data[4*i] << 8 | map.data[4*i+1]); // Big endian
      rms_power += audio_point*audio_point;
      audio_buffer[2*i]   = map.data[4*i]; // Skip two bytes
      audio_buffer[2*i+1] = map.data[4*i+1];
      // printf("%02x", map.data[i]);
      // if (i % 4 == 0)
      //   printf(" ");
      // if (i % 8 == 0)
      //   printf(" ");
      // if (i % 16 == 0)
      //   printf("\n");
    }
    rms_power = sqrt(rms_power/i); // i = N
    GST_DEBUG (" rms_power %f audio_point %d",
                rms_power, audio_point);

    OV_ZMQ_STRBUF("ov_asrc "); // Format str_buffer
    // s_send (ov_zmq_publisher, str_buffer);
    zmq_send (ov_zmq_publisher, str_buffer, strlen (str_buffer), 0);
    // zmq_send (ov_zmq_publisher, str_buffer, strlen (str_buffer), ZMQ_SNDMORE);
//    zmq_send (ov_zmq_publisher, audio_buffer, map.size/2, 0);

    gst_buffer_unmap (buffer, &map);
  } else {
    GST_DEBUG("Failed to map buffer.");
  }

  // GST_PAD_PROBE_INFO_DATA (info) = buffer;

  return GST_PAD_PROBE_OK;
}
