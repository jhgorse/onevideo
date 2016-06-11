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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#define AUDIO_RATE 48000
#define frame_buffer_size (AUDIO_RATE * 10 * 2 * 2 / 1000)      // 48 khz, 10 ms, 2 channels, 2 byte sample
#define samples_per_frame (AUDIO_RATE * 10 / 1000)

/* Read a raw audio file (48KHz sample frequency, 16bit PCM, stereo)
 * from stdin, echo cancel it and write it to stdout
 */
int
main (int argc, char *argv[])
{
  // Stereo interleaved, but really mono
  int16_t file_buffer[samples_per_frame * 2];
  char near_buffer[frame_buffer_size];
  char far_buffer[frame_buffer_size];

  fprintf (stderr, "usage: audio-processing-test <in.pcm >out.pcm\n");

  ov_local_peer_audio_processing_init (AUDIO_RATE, 2);

  int read_size, total_size = 0;
  while (1) {
    read_size =
        fread (file_buffer, sizeof (int16_t), 2 * samples_per_frame, stdin);
    total_size += read_size;
    if (read_size != (2 * samples_per_frame)) {
      fprintf (stderr, "File End. Total %d Bytes.\n", total_size << 1);
      break;
    }
    for (int i = 0; i < samples_per_frame; i++) {
      far_buffer[4 * i + 0] = (char) file_buffer[2 * i];        // Left channel to stereo far end
      far_buffer[4 * i + 1] = (char) file_buffer[2 * i] >> 8;
      far_buffer[4 * i + 2] = (char) file_buffer[2 * i];        // duplicated 
      far_buffer[4 * i + 3] = (char) file_buffer[2 * i] >> 8;

      near_buffer[4 * i + 0] = (char) file_buffer[2 * i + 1];   // Right channel to stereo near end
      near_buffer[4 * i + 1] = (char) file_buffer[2 * i + 1] >> 8;
      near_buffer[4 * i + 2] = (char) file_buffer[2 * i + 1];   // duplicated
      near_buffer[4 * i + 3] = (char) file_buffer[2 * i + 1] >> 8;
    }

    ov_local_peer_audio_processing_far_speech_update (far_buffer,
        frame_buffer_size);
    ov_local_peer_audio_processing_near_speech_update (near_buffer,
        frame_buffer_size);

    fwrite (near_buffer, sizeof (int16_t), 2 * samples_per_frame, stdout);
  }

  fprintf (stderr, "Done\n");
  fflush (NULL);
  return 0;
}
