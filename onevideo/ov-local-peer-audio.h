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

 #include "lib.h"
 #include "ov-local-peer.h"

// typedef double FILTER_TYPE_DATA;
//
// /* Bring your own coefficients and data history buffers. */
// typedef struct {
//   // char name[32];      // Just in case you use these things
//   size_t           taps;   // How many taps we have
//   FILTER_TYPE_DATA *coef;  // Pointer to array of coefficients, taps long
//   FILTER_TYPE_DATA *x;     // History of inputs, taps long
//   size_t           ix;     // Iterator
// } FIR_FILTER;

// GST_TIME_FORMAT/ARGS for OV
#define OV_GST_TIME_FORMAT "02u.%09u"
#define OV_GST_TIME_ARGS(t) \
        GST_CLOCK_TIME_IS_VALID (t) ? \
        (guint) ((((GstClockTime)(t)) / GST_SECOND) % 60) : 99, \
        GST_CLOCK_TIME_IS_VALID (t) ? \
        (guint) (((GstClockTime)(t)) % GST_SECOND) : 999999999

#define OV_ZMQ_STRBUF(STUFF)        sprintf (str_buffer, STUFF " %" OV_GST_TIME_FORMAT " duration %lu offset %lu offset_end %lu offset_delta %lu %lu", \
          OV_GST_TIME_ARGS(gst_util_get_timestamp()), buffer->duration, buffer->offset, buffer->offset_end, buffer->offset_end - buffer->offset, map.size );


GstPadProbeReturn ov_asink_input_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data);
GstPadProbeReturn ov_asrc_input_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data);