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

#include "ov-local-peer-audio-processing.h"

#include <iostream>
#include <sys/select.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <string.h>

#include <gst/gst.h>
#include <gio/gio.h>

#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/modules/audio_processing/aec/include/echo_cancellation.h"
#include "webrtc/modules/interface/module_common_types.h"
#include "webrtc/system_wrappers/include/trace.h"
#include "webrtc/common_audio/include/audio_util.h"
#include "webrtc/typedefs.h"

static webrtc::AudioProcessing* apm;
static int analog_level;
static float *far_buffer[2]; // Max num channels
static float *near_buffer[2];
static int samples_per_frame; // Also the length of each buffer

// Update the floats from nt16 pcm
extern "C" void
deinterleave_pcm_to_float(const char *src_data, float **dst, size_t size) {
  for (int i=0;i<size/4;i++) { // Start at offset
    dst[0][i] = webrtc::S16ToFloat((short)(src_data[4*i+1] << 8 | src_data[4*i])); // Little endian
    dst[1][i] = webrtc::S16ToFloat((short)(src_data[4*i+3] << 8 | src_data[4*i+2])); // Little endian
  }
}

// Update the int16 pcm from floats
extern "C" void
interleave_float_to_pcm(float **src_data, char *dst, size_t size) {
  int16_t audio_point;

  for(int i=0;i<size/4;i++) {
    audio_point = webrtc::FloatToS16(src_data[0][i]);
    dst[4*i] = (char)(0xff & (audio_point >> 0));
    dst[4*i+1] = (char)(0xff & (audio_point >> 8));

    audio_point = webrtc::FloatToS16(src_data[1][i]);
    dst[4*i+2] = (char)(0xff & (audio_point >> 0));
    dst[4*i+3] = (char)(0xff & (audio_point >> 8));
  }
}

// Pass 10 ms asink audio buffer mem(addr,size) to ProcessReverseStream()
//   Assumes PCM: interleaved 16 bit LE signed int
extern "C" void
ov_local_peer_audio_processing_far_speech_update(char *data, size_t size) {
  webrtc::StreamConfig config(AUDIO_RATE, 2, false); // TODO: Fix this

  deinterleave_pcm_to_float(data, far_buffer, size);

  //apm->set_stream_delay_ms(-80);
  if (apm->ProcessReverseStream(far_buffer, config, config, far_buffer) !=
    webrtc::AudioProcessing::kNoError)
      GST_ERROR("ProcessReverseStream error");
  // apm->gain_control()->set_stream_analog_level(analog_level);
}

// Pass 10 ms asrc audio buffer mem(addr,size) to ProcessStream()
extern "C" void
ov_local_peer_audio_processing_near_speech_update(char *data, size_t size) {
  webrtc::StreamConfig config(AUDIO_RATE, 2, false); // TODO: Fix this

  deinterleave_pcm_to_float(data, near_buffer, size);

  apm->ProcessStream(near_buffer, config, config, near_buffer);
  // analog_level = apm->gain_control()->stream_analog_level();
  // has_voice = apm->voice_detection()->stream_has_voice();

  interleave_float_to_pcm(near_buffer, data, size);
}


// Initialize and configure the AudioProcessing unit
//   2x10 ms buffers
//   4x configs: sample rates, channels. (in/out for each node)
//   Library options
extern "C" void
ov_local_peer_audio_processing_init(int sample_rate_hz, int num_channels) {
  webrtc::ProcessingConfig processing_config;
  webrtc::Config config;
  webrtc::StreamConfig stream_config(sample_rate_hz, num_channels, false);
  samples_per_frame = sample_rate_hz * 10 / 1000; // 10 ms frame

  size_t ch_buffer_size = samples_per_frame * sizeof(float);
  for (int i=0;i<num_channels;i++) {
    far_buffer[i]  = (float *)malloc( ch_buffer_size );
    near_buffer[i] = (float *)malloc( ch_buffer_size );
    memset(far_buffer[i],  0, ch_buffer_size);
    memset(near_buffer[i], 0, ch_buffer_size);
  }


  analog_level = 0;

  config.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(true));
  config.Set<webrtc::ExperimentalAgc>(new webrtc::ExperimentalAgc(true, 85));  // Start volume 85
  //DelayAgnostic
  //ExperimentalNs
  //Intelligibility

  apm = webrtc::AudioProcessing::Create(config);

  processing_config = {
    stream_config, /* input stream mic/near */
    stream_config, /* output stream */
    stream_config, /* reverse input stream speaker/far */
    stream_config, /* reverse output stream */
  };

  if (apm->Initialize(processing_config) != webrtc::AudioProcessing::kNoError) {
    GST_ERROR("Error initialising audio processing module");
    goto fail;
  }
   apm->high_pass_filter()->Enable(true);

   #if 1
     apm->echo_cancellation()->enable_drift_compensation(false);
     apm->echo_cancellation()->Enable(true);
   #else
     apm->echo_control_mobile()->set_routing_mode(static_cast<webrtc::EchoControlMobile::RoutingMode>(rm));
     apm->echo_control_mobile()->enable_comfort_noise(cn);
     apm->echo_control_mobile()->Enable(true);
   #endif

   apm->set_stream_delay_ms(-80); // Initial delay

   apm->noise_suppression()->set_level(webrtc::NoiseSuppression::kHigh);
   apm->noise_suppression()->Enable(true);

   // Adaptive gain control
  //  if (agc || dgc)
  #if 0
     if (mobile && rm <= webrtc::EchoControlMobile::kEarpiece) {
       /* Maybe this should be a knob, but we've got a lot of knobs already */
       apm->gain_control()->set_mode(webrtc::GainControl::kFixedDigital);
       ec->params.webrtc.agc = false;
     } else if (dgc) {
       apm->gain_control()->set_mode(webrtc::GainControl::kAdaptiveDigital);
       ec->params.webrtc.agc = false;
     } else {
       apm->gain_control()->set_mode(webrtc::GainControl::kAdaptiveAnalog);
       if (apm->gain_control()->set_analog_level_limits(0, 255) !=
           webrtc::AudioProcessing::kNoError) {
         pa_log("Failed to initialise AGC");
         goto fail;
       }
       ec->params.webrtc.agc = true;
     }

     apm->gain_control()->Enable(true);
  #endif

  apm->voice_detection()->Enable(true);

   return;
fail:
  GST_ERROR("Something failed");
  return;
}

extern "C" void
ov_local_peer_audio_processing_deinit() {
  if (apm) {
      delete apm;
      apm = NULL;
  }
  return;
}
