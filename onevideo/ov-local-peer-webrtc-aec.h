/*  vim: set sts=2 sw=2 et :
 *
 *  Copyright (C) 2016 G3 Institute
 *  Author(s): Joe Gorse <jhgorse@gmail.com>,
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

#ifndef ov_local_peer_webrtc_aec_h
#define ov_local_peer_webrtc_aec_h

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>

typedef uint32_t ov_volume_t;

#define OV_VOLUME_NORM ((ov_volume_t) 0x10000U)

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulsecore/core.h>
#include <pulsecore/macro.h>

/* Common data structures */

typedef struct pa_echo_canceller_msg pa_echo_canceller_msg;

typedef struct pa_echo_canceller_params pa_echo_canceller_params;

struct pa_echo_canceller_params {
  union {
    struct {
      pa_sample_spec out_ss;
    } null;
#ifdef HAVE_WEBRTC
    struct {
      /* This is a void* so that we don't have to convert this whole file
       * to C++ linkage. apm is a pointer to an AudioProcessing object */
      void *apm;
      unsigned int blocksize; /* in frames */
      pa_sample_spec rec_ss, play_ss, out_ss;
      float *rec_buffer[PA_CHANNELS_MAX], *play_buffer[PA_CHANNELS_MAX]; /* for deinterleaved buffers */
      void *trace_callback;
      bool agc;
      bool first;
      unsigned int agc_start_volume;
    } webrtc;
#endif
    /* each canceller-specific structure goes here */
  };
  
  /* Set this if canceller can do drift compensation. Also see set_drift()
   * below */
  bool drift_compensation;
};

typedef struct pa_echo_canceller pa_echo_canceller;

struct pa_echo_canceller {
  /* Initialise canceller engine. */
  bool   (*init)                      (pa_core *c,
                                       pa_echo_canceller *ec,
                                       pa_sample_spec *rec_ss,
                                       pa_channel_map *rec_map,
                                       pa_sample_spec *play_ss,
                                       pa_channel_map *play_map,
                                       pa_sample_spec *out_ss,
                                       pa_channel_map *out_map,
                                       uint32_t *nframes,
                                       const char *args);
  
  /* You should have only one of play()+record() or run() set. The first
   * works under the assumption that you'll handle buffering and matching up
   * samples yourself. If you set run(), module-echo-cancel will handle
   * synchronising the playback and record streams. */
  
  /* Feed the engine 'nframes' playback frames. */
  void        (*play)                 (pa_echo_canceller *ec, const uint8_t *play);
  /* Feed the engine 'nframes' record frames. nframes processed frames are
   * returned in out. */
  void        (*record)               (pa_echo_canceller *ec, const uint8_t *rec, uint8_t *out);
  /* Feed the engine nframes playback and record frames, with a reasonable
   * effort at keeping the two in sync. nframes processed frames are
   * returned in out. */
  void        (*run)                  (pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out);
  
  /* Optional callback to set the drift, expressed as the ratio of the
   * difference in number of playback and capture samples to the number of
   * capture samples, for some instant of time. This is used only if the
   * canceller signals that it supports drift compensation, and is called
   * before record(). The actual implementation needs to derive drift based
   * on point samples -- the individual values are not accurate enough to use
   * as-is. */
  /* NOTE: the semantics of this function might change in the future. */
  void        (*set_drift)            (pa_echo_canceller *ec, float drift);
  
  /* Free up resources. */
  void        (*done)                 (pa_echo_canceller *ec);
  
  /* Structure with common and engine-specific canceller parameters. */
  pa_echo_canceller_params params;
  
  /* msgobject that can be used to send messages back to the main thread */
  pa_echo_canceller_msg *msg;
};

/* Functions to be used by the canceller analog gain control routines */
pa_volume_t pa_echo_canceller_get_capture_volume(pa_echo_canceller *ec);
void pa_echo_canceller_set_capture_volume(pa_echo_canceller *ec, pa_volume_t volume);

/* Computes EC block size in frames (rounded down to nearest power-of-2) based
 * on sample rate and milliseconds. */
uint32_t pa_echo_canceller_blocksize_power2(unsigned rate, unsigned ms);

/* Null canceller functions */
bool pa_null_ec_init(pa_core *c, pa_echo_canceller *ec,
                     pa_sample_spec *rec_ss, pa_channel_map *rec_map,
                     pa_sample_spec *play_ss, pa_channel_map *play_map,
                     pa_sample_spec *out_ss, pa_channel_map *out_map,
                     uint32_t *nframes, const char *args);
void pa_null_ec_run(pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out);
void pa_null_ec_done(pa_echo_canceller *ec);

#ifdef HAVE_WEBRTC
/* WebRTC canceller functions */
PA_C_DECL_BEGIN
bool pa_webrtc_ec_init(pa_core *c, pa_echo_canceller *ec,
                       pa_sample_spec *rec_ss, pa_channel_map *rec_map,
                       pa_sample_spec *play_ss, pa_channel_map *play_map,
                       pa_sample_spec *out_ss, pa_channel_map *out_map,
                       uint32_t *nframes, const char *args);
void pa_webrtc_ec_play(pa_echo_canceller *ec, const uint8_t *play);
void pa_webrtc_ec_record(pa_echo_canceller *ec, const uint8_t *rec, uint8_t *out);
void pa_webrtc_ec_set_drift(pa_echo_canceller *ec, float drift);
void pa_webrtc_ec_run(pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out);
void pa_webrtc_ec_done(pa_echo_canceller *ec);
PA_C_DECL_END
#endif

#endif /* ov_local_peer_webrtc_aec_h */
