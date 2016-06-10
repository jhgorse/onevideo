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

#ifndef ov_local_peer_audio_processing_h
#define ov_local_peer_audio_processing_h

#include <unistd.h>

#define AUDIO_RATE 48000

#ifdef __cplusplus
extern "C" {
#endif

  // Initialize and configure the AudioProcessing unit
  //   4x configs: sample rates, channels. (in/out for each node)
  //   Library options
  void ov_local_peer_audio_processing_init(int sample_rate_hz, int num_channels);
  void ov_local_peer_audio_processing_deinit();

  // Pass a 10 ms asink audio buffer mem(addr,size) to ProcessReverseStream()
  //   Assumes PCM: interleaved 16 bit LE signed int
  void ov_local_peer_audio_processing_far_speech_update(char *data, size_t size);

  // Pass a 10 ms asrc audio buffer mem(addr,size) to ProcessStream()
  void ov_local_peer_audio_processing_near_speech_update(char *data, size_t size);
  // Update delay_from_asink_to_asrc if level > threshold
  //   access the AudioProcessing buffers...
  //   make our own...
//  ov_local_peer_audio_processing_far_to_near_speech_delay()

#ifdef __cplusplus
}
#endif

#endif /* ov_local_peer_audio_processing_h */
