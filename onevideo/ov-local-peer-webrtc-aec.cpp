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

#include "ov-local-peer-webrtc-aec.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "echo-cancel.h"

#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>
#include <webrtc/system_wrappers/include/trace.h>

#define BLOCK_SIZE_US 10000

#define DEFAULT_HIGH_PASS_FILTER true
#define DEFAULT_NOISE_SUPPRESSION true
#define DEFAULT_ANALOG_GAIN_CONTROL true
#define DEFAULT_DIGITAL_GAIN_CONTROL false
#define DEFAULT_MOBILE false
#define DEFAULT_ROUTING_MODE "speakerphone"
#define DEFAULT_COMFORT_NOISE true
#define DEFAULT_DRIFT_COMPENSATION false
#define DEFAULT_VAD true
#define DEFAULT_EXTENDED_FILTER false
#define DEFAULT_INTELLIGIBILITY_ENHANCER false
#define DEFAULT_EXPERIMENTAL_AGC false
#define DEFAULT_AGC_START_VOLUME 85
#define DEFAULT_BEAMFORMING false
#define DEFAULT_TRACE false

#define WEBRTC_AGC_MAX_VOLUME 255

static const char* const valid_modargs[] = {
  "high_pass_filter",
  "noise_suppression",
  "analog_gain_control",
  "digital_gain_control",
  "mobile",
  "routing_mode",
  "comfort_noise",
  "drift_compensation",
  "voice_detection",
  "extended_filter",
  "intelligibility_enhancer",
  "experimental_agc",
  "agc_start_volume",
  "beamforming",
  "mic_geometry", /* documented in parse_mic_geometry() */
  "target_direction", /* documented in parse_mic_geometry() */
  "trace",
  NULL
};

static int ov_streq (const char * str1, const char * str2) {
  return strcmp (str1, str2) == 0;
}

static int routing_mode_from_string(const char *rmode) {
  if (ov_streq(rmode, "quiet-earpiece-or-headset"))
    return webrtc::EchoControlMobile::kQuietEarpieceOrHeadset;
  else if (ov_streq(rmode, "earpiece"))
    return webrtc::EchoControlMobile::kEarpiece;
  else if (ov_streq(rmode, "loud-earpiece"))
    return webrtc::EchoControlMobile::kLoudEarpiece;
  else if (ov_streq(rmode, "speakerphone"))
    return webrtc::EchoControlMobile::kSpeakerphone;
  else if (ov_streq(rmode, "loud-speakerphone"))
    return webrtc::EchoControlMobile::kLoudSpeakerphone;
  else
    return -1;
}

class PaWebrtcTraceCallback : public webrtc::TraceCallback {
  void Print(webrtc::TraceLevel level, const char *message, int length)
  {
    if (level & webrtc::kTraceError || level & webrtc::kTraceCritical)
      GST_DEBUG(message);
    else if (level & webrtc::kTraceWarning)
      GST_WARNING(message);
    else if (level & webrtc::kTraceInfo)
      GST_INFO(message);
    else
      GST_DEBUG(message);
  }
};

static int webrtc_volume_from_ov(ov_volume_t v)
{
  return (v * WEBRTC_AGC_MAX_VOLUME) / OV_VOLUME_NORM;
}

static ov_volume_t webrtc_volume_to_ov(int v)
{
  return (v * OV_VOLUME_NORM) / WEBRTC_AGC_MAX_VOLUME;
}

static void webrtc_ec_fixate_spec(pa_sample_spec *rec_ss, pa_channel_map *rec_map,
                                  pa_sample_spec *play_ss, pa_channel_map *play_map,
                                  pa_sample_spec *out_ss, pa_channel_map *out_map,
                                  bool beamforming)
{
  rec_ss->format = PA_SAMPLE_FLOAT32NE;
  play_ss->format = PA_SAMPLE_FLOAT32NE;
  
  /* AudioProcessing expects one of the following rates */
  if (rec_ss->rate >= 48000)
    rec_ss->rate = 48000;
  else if (rec_ss->rate >= 32000)
    rec_ss->rate = 32000;
  else if (rec_ss->rate >= 16000)
    rec_ss->rate = 16000;
  else
    rec_ss->rate = 8000;
  
  *out_ss = *rec_ss;
  *out_map = *rec_map;
  
  if (beamforming) {
    /* The beamformer gives us a single channel */
    out_ss->channels = 1;
    pa_channel_map_init_mono(out_map);
  }
  
  /* Playback stream rate needs to be the same as capture */
  play_ss->rate = rec_ss->rate;
}

static bool parse_point(const char **point, float (&f)[3]) {
  int ret, length;
  
  ret = sscanf(*point, "%g,%g,%g%n", &f[0], &f[1], &f[2], &length);
  if (ret != 3)
    return false;
  
  /* Consume the bytes we've read so far */
  *point += length;
  
  return true;
}

static bool parse_mic_geometry(const char **mic_geometry, std::vector<webrtc::Point>& geometry) {
  /* The microphone geometry is expressed as cartesian point form:
   *   x1,y1,z1,x2,y2,z2,...
   *
   * Where x1,y1,z1 is the position of the first microphone with regards to
   * the array's "center", x2,y2,z2 the position of the second, and so on.
   *
   * 'x' is the horizontal coordinate, with positive values being to the
   * right from the mic array's perspective.
   *
   * 'y' is the depth coordinate, with positive values being in front of the
   * array.
   *
   * 'z' is the vertical coordinate, with positive values being above the
   * array.
   *
   * All distances are in meters.
   */
  
  /* The target direction is expected to be in spherical point form:
   *   a,e,r
   *
   * Where 'a is the azimuth of the first mic channel, 'e' its elevation,
   * and 'r' the radius.
   *
   * 0 radians azimuth is to the right of the array, and positive angles
   * move in a counter-clockwise direction.
   *
   * 0 radians elevation is horizontal w.r.t. the array, and positive
   * angles go upwards.
   *
   * radius is distance from the array center in meters.
   */
  
  int i;
  float f[3];
  
  for (i = 0; i < geometry.size(); i++) {
    if (!parse_point(mic_geometry, f)) {
      GST_LOG("Failed to parse channel %d in mic_geometry", i);
      return false;
    }
    
    /* Except for the last point, we should have a trailing comma */
    if (i != geometry.size() - 1) {
      if (**mic_geometry != ',') {
        GST_LOG("Failed to parse channel %d in mic_geometry", i);
        return false;
      }
      
      (*mic_geometry)++;
    }
    
    GST_DEBUG("Got mic #%d position: (%g, %g, %g)", i, f[0], f[1], f[2]);
    
    geometry[i].c[0] = f[0];
    geometry[i].c[1] = f[1];
    geometry[i].c[2] = f[2];
  }
  
  if (**mic_geometry != '\0') {
    GST_LOG("Failed to parse mic_geometry value: more parameters than expected");
    return false;
  }
  
  return true;
}

bool pa_webrtc_ec_init(pa_core *c, pa_echo_canceller *ec,
                       pa_sample_spec *rec_ss, pa_channel_map *rec_map,
                       pa_sample_spec *play_ss, pa_channel_map *play_map,
                       pa_sample_spec *out_ss, pa_channel_map *out_map,
                       uint32_t *nframes, const char *args) {
  webrtc::AudioProcessing *apm = NULL;
  webrtc::ProcessingConfig pconfig;
  webrtc::Config config;
  bool hpf, ns, agc, dgc, mobile, cn, vad, ext_filter, intelligibility, experimental_agc, beamforming;
  int rm = -1, i;
  uint32_t agc_start_volume;
  pa_modargs *ma;
  bool trace = false;
  
  if (!(ma = pa_modargs_new(args, valid_modargs))) {
    GST_LOG("Failed to parse submodule arguments.");
    goto fail;
  }
  
  hpf = DEFAULT_HIGH_PASS_FILTER;
  if (pa_modargs_get_value_boolean(ma, "high_pass_filter", &hpf) < 0) {
    GST_LOG("Failed to parse high_pass_filter value");
    goto fail;
  }
  
  ns = DEFAULT_NOISE_SUPPRESSION;
  if (pa_modargs_get_value_boolean(ma, "noise_suppression", &ns) < 0) {
    GST_LOG("Failed to parse noise_suppression value");
    goto fail;
  }
  
  agc = DEFAULT_ANALOG_GAIN_CONTROL;
  if (pa_modargs_get_value_boolean(ma, "analog_gain_control", &agc) < 0) {
    GST_LOG("Failed to parse analog_gain_control value");
    goto fail;
  }
  
  dgc = agc ? false : DEFAULT_DIGITAL_GAIN_CONTROL;
  if (pa_modargs_get_value_boolean(ma, "digital_gain_control", &dgc) < 0) {
    GST_LOG("Failed to parse digital_gain_control value");
    goto fail;
  }
  
  if (agc && dgc) {
    GST_LOG("You must pick only one between analog and digital gain control");
    goto fail;
  }
  
  mobile = DEFAULT_MOBILE;
  if (pa_modargs_get_value_boolean(ma, "mobile", &mobile) < 0) {
    GST_LOG("Failed to parse mobile value");
    goto fail;
  }
  
  ec->params.drift_compensation = DEFAULT_DRIFT_COMPENSATION;
  if (pa_modargs_get_value_boolean(ma, "drift_compensation", &ec->params.drift_compensation) < 0) {
    GST_LOG("Failed to parse drift_compensation value");
    goto fail;
  }
  
  if (mobile) {
    if (ec->params.drift_compensation) {
      GST_LOG("Can't use drift_compensation in mobile mode");
      goto fail;
    }
    
    if ((rm = routing_mode_from_string(pa_modargs_get_value(ma, "routing_mode", DEFAULT_ROUTING_MODE))) < 0) {
      GST_LOG("Failed to parse routing_mode value");
      goto fail;
    }
    
    cn = DEFAULT_COMFORT_NOISE;
    if (pa_modargs_get_value_boolean(ma, "comfort_noise", &cn) < 0) {
      GST_LOG("Failed to parse cn value");
      goto fail;
    }
  } else {
    if (pa_modargs_get_value(ma, "comfort_noise", NULL) || pa_modargs_get_value(ma, "routing_mode", NULL)) {
      GST_LOG("The routing_mode and comfort_noise options are only valid with mobile=true");
      goto fail;
    }
  }
  
  vad = DEFAULT_VAD;
  if (pa_modargs_get_value_boolean(ma, "voice_detection", &vad) < 0) {
    GST_LOG("Failed to parse voice_detection value");
    goto fail;
  }
  
  ext_filter = DEFAULT_EXTENDED_FILTER;
  if (pa_modargs_get_value_boolean(ma, "extended_filter", &ext_filter) < 0) {
    GST_LOG("Failed to parse extended_filter value");
    goto fail;
  }
  
  intelligibility = DEFAULT_INTELLIGIBILITY_ENHANCER;
  if (pa_modargs_get_value_boolean(ma, "intelligibility_enhancer", &intelligibility) < 0) {
    GST_LOG("Failed to parse intelligibility_enhancer value");
    goto fail;
  }
  
  experimental_agc = DEFAULT_EXPERIMENTAL_AGC;
  if (pa_modargs_get_value_boolean(ma, "experimental_agc", &experimental_agc) < 0) {
    GST_LOG("Failed to parse experimental_agc value");
    goto fail;
  }
  
  agc_start_volume = DEFAULT_AGC_START_VOLUME;
  if (pa_modargs_get_value_u32(ma, "agc_start_volume", &agc_start_volume) < 0) {
    GST_LOG("Failed to parse agc_start_volume value");
    goto fail;
  }
  if (agc_start_volume > WEBRTC_AGC_MAX_VOLUME) {
    GST_LOG("AGC start volume must not exceed %u", WEBRTC_AGC_MAX_VOLUME);
    goto fail;
  }
  ec->params.webrtc.agc_start_volume = agc_start_volume;
  
  beamforming = DEFAULT_BEAMFORMING;
  if (pa_modargs_get_value_boolean(ma, "beamforming", &beamforming) < 0) {
    GST_LOG("Failed to parse beamforming value");
    goto fail;
  }
  
  if (ext_filter)
    config.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(true));
  if (intelligibility)
    GST_WARNING("The intelligibility enhancer is not currently supported");
  if (experimental_agc)
    config.Set<webrtc::ExperimentalAgc>(new webrtc::ExperimentalAgc(true, ec->params.webrtc.agc_start_volume));
  
  trace = DEFAULT_TRACE;
  if (pa_modargs_get_value_boolean(ma, "trace", &trace) < 0) {
    GST_LOG("Failed to parse trace value");
    goto fail;
  }
  
  if (trace) {
    webrtc::Trace::CreateTrace();
    webrtc::Trace::set_level_filter(webrtc::kTraceAll);
    ec->params.webrtc.trace_callback = new PaWebrtcTraceCallback();
    webrtc::Trace::SetTraceCallback((PaWebrtcTraceCallback *) ec->params.webrtc.trace_callback);
  }
  
  webrtc_ec_fixate_spec(rec_ss, rec_map, play_ss, play_map, out_ss, out_map, beamforming);
  
  /* We do this after fixate because we need the capture channel count */
  if (beamforming) {
    std::vector<webrtc::Point> geometry(rec_ss->channels);
    webrtc::SphericalPointf direction(0.0f, 0.0f, 0.0f);
    const char *mic_geometry, *target_direction;
    
    if (!(mic_geometry = pa_modargs_get_value(ma, "mic_geometry", NULL))) {
      GST_LOG("mic_geometry must be set if beamforming is enabled");
      goto fail;
    }
    
    if (!parse_mic_geometry(&mic_geometry, geometry)) {
      GST_LOG("Failed to parse mic_geometry value");
      goto fail;
    }
    
    if ((target_direction = pa_modargs_get_value(ma, "target_direction", NULL))) {
      float f[3];
      
      if (!parse_point(&target_direction, f)) {
        GST_LOG("Failed to parse target_direction value");
        goto fail;
      }
      
      if (*target_direction != '\0') {
        GST_LOG("Failed to parse target_direction value: more parameters than expected");
        goto fail;
      }
      
#define IS_ZERO(f) ((f) < 0.000001 && (f) > -0.000001)
      
      if (!IS_ZERO(f[1]) || !IS_ZERO(f[2])) {
        GST_LOG("The beamformer currently only supports targeting along the azimuth");
        goto fail;
      }
      
      direction.s[0] = f[0];
      direction.s[1] = f[1];
      direction.s[2] = f[2];
    }
    
    if (!target_direction)
      config.Set<webrtc::Beamforming>(new webrtc::Beamforming(true, geometry));
    else
      config.Set<webrtc::Beamforming>(new webrtc::Beamforming(true, geometry, direction));
  }
  
  apm = webrtc::AudioProcessing::Create(config);
  
  pconfig = {
    webrtc::StreamConfig(rec_ss->rate, rec_ss->channels, false), /* input stream */
    webrtc::StreamConfig(out_ss->rate, out_ss->channels, false), /* output stream */
    webrtc::StreamConfig(play_ss->rate, play_ss->channels, false), /* reverse input stream */
    webrtc::StreamConfig(play_ss->rate, play_ss->channels, false), /* reverse output stream */
  };
  if (apm->Initialize(pconfig) != webrtc::AudioProcessing::kNoError) {
    GST_LOG("Error initialising audio processing module");
    goto fail;
  }
  
  if (hpf)
    apm->high_pass_filter()->Enable(true);
  
  if (!mobile) {
    apm->echo_cancellation()->enable_drift_compensation(ec->params.drift_compensation);
    apm->echo_cancellation()->Enable(true);
  } else {
    apm->echo_control_mobile()->set_routing_mode(static_cast<webrtc::EchoControlMobile::RoutingMode>(rm));
    apm->echo_control_mobile()->enable_comfort_noise(cn);
    apm->echo_control_mobile()->Enable(true);
  }
  
  if (ns) {
    apm->noise_suppression()->set_level(webrtc::NoiseSuppression::kHigh);
    apm->noise_suppression()->Enable(true);
  }
  
  if (agc || dgc) {
    if (mobile && rm <= webrtc::EchoControlMobile::kEarpiece) {
      /* Maybe this should be a knob, but we've got a lot of knobs already */
      apm->gain_control()->set_mode(webrtc::GainControl::kFixedDigital);
      ec->params.webrtc.agc = false;
    } else if (dgc) {
      apm->gain_control()->set_mode(webrtc::GainControl::kAdaptiveDigital);
      ec->params.webrtc.agc = false;
    } else {
      apm->gain_control()->set_mode(webrtc::GainControl::kAdaptiveAnalog);
      if (apm->gain_control()->set_analog_level_limits(0, WEBRTC_AGC_MAX_VOLUME) !=
          webrtc::AudioProcessing::kNoError) {
        GST_LOG("Failed to initialise AGC");
        goto fail;
      }
      ec->params.webrtc.agc = true;
    }
    
    apm->gain_control()->Enable(true);
  }
  
  if (vad)
    apm->voice_detection()->Enable(true);
  
  ec->params.webrtc.apm = apm;
  ec->params.webrtc.rec_ss = *rec_ss;
  ec->params.webrtc.play_ss = *play_ss;
  ec->params.webrtc.out_ss = *out_ss;
  ec->params.webrtc.blocksize = (uint64_t) out_ss->rate * BLOCK_SIZE_US / PA_USEC_PER_SEC;
  *nframes = ec->params.webrtc.blocksize;
  ec->params.webrtc.first = true;
  
  for (i = 0; i < rec_ss->channels; i++)
    ec->params.webrtc.rec_buffer[i] = pa_xnew(float, *nframes);
  for (i = 0; i < play_ss->channels; i++)
    ec->params.webrtc.play_buffer[i] = pa_xnew(float, *nframes);
  
  pa_modargs_free(ma);
  return true;
  
fail:
  if (ma)
    pa_modargs_free(ma);
  if (ec->params.webrtc.trace_callback) {
    webrtc::Trace::ReturnTrace();
    delete ((PaWebrtcTraceCallback *) ec->params.webrtc.trace_callback);
  } if (apm)
    delete apm;
  
  return false;
}

void pa_webrtc_ec_play(pa_echo_canceller *ec, const uint8_t *play) {
  webrtc::AudioProcessing *apm = (webrtc::AudioProcessing*)ec->params.webrtc.apm;
  const pa_sample_spec *ss = &ec->params.webrtc.play_ss;
  int n = ec->params.webrtc.blocksize;
  float **buf = ec->params.webrtc.play_buffer;
  webrtc::StreamConfig config(ss->rate, ss->channels, false);
  
  pa_deinterleave(play, (void **) buf, ss->channels, pa_sample_size(ss), n);
  
  pa_assert_se(apm->ProcessReverseStream(buf, config, config, buf) == webrtc::AudioProcessing::kNoError);
  
  /* FIXME: If ProcessReverseStream() makes any changes to the audio, such as
   * applying intelligibility enhancement, those changes don't have any
   * effect. This function is called at the source side, but the processing
   * would have to be done in the sink to be able to feed the processed audio
   * to speakers. */
}

void pa_webrtc_ec_record(pa_echo_canceller *ec, const uint8_t *rec, uint8_t *out) {
  webrtc::AudioProcessing *apm = (webrtc::AudioProcessing*)ec->params.webrtc.apm;
  const pa_sample_spec *rec_ss = &ec->params.webrtc.rec_ss;
  const pa_sample_spec *out_ss = &ec->params.webrtc.out_ss;
  float **buf = ec->params.webrtc.rec_buffer;
  int n = ec->params.webrtc.blocksize;
  int old_volume, new_volume;
  webrtc::StreamConfig rec_config(rec_ss->rate, rec_ss->channels, false);
  webrtc::StreamConfig out_config(out_ss->rate, out_ss->channels, false);
  
  pa_deinterleave(rec, (void **) buf, rec_ss->channels, pa_sample_size(rec_ss), n);
  
  if (ec->params.webrtc.agc) {
    ov_volume_t v = pa_echo_canceller_get_capture_volume(ec);
    old_volume = webrtc_volume_from_pa(v);
    apm->gain_control()->set_stream_analog_level(old_volume);
  }
  
  apm->set_stream_delay_ms(0);
  pa_assert_se(apm->ProcessStream(buf, rec_config, out_config, buf) == webrtc::AudioProcessing::kNoError);
  
  if (ec->params.webrtc.agc) {
    if (PA_UNLIKELY(ec->params.webrtc.first)) {
      /* We start at a sane default volume (taken from the Chromium
       * condition on the experimental AGC in audio_processing.h). This is
       * needed to make sure that there's enough energy in the capture
       * signal for the AGC to work */
      ec->params.webrtc.first = false;
      new_volume = ec->params.webrtc.agc_start_volume;
    } else {
      new_volume = apm->gain_control()->stream_analog_level();
    }
    
    if (old_volume != new_volume)
      pa_echo_canceller_set_capture_volume(ec, webrtc_volume_to_pa(new_volume));
  }
  
  pa_interleave((const void **) buf, out_ss->channels, out, pa_sample_size(out_ss), n);
}

void pa_webrtc_ec_set_drift(pa_echo_canceller *ec, float drift) {
  webrtc::AudioProcessing *apm = (webrtc::AudioProcessing*)ec->params.webrtc.apm;
  
  apm->echo_cancellation()->set_stream_drift_samples(drift * ec->params.webrtc.blocksize);
}

void pa_webrtc_ec_run(pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out) {
  pa_webrtc_ec_play(ec, play);
  pa_webrtc_ec_record(ec, rec, out);
}

void pa_webrtc_ec_done(pa_echo_canceller *ec) {
  int i;
  
  if (ec->params.webrtc.trace_callback) {
    webrtc::Trace::ReturnTrace();
    delete ((PaWebrtcTraceCallback *) ec->params.webrtc.trace_callback);
  }
  
  if (ec->params.webrtc.apm) {
    delete (webrtc::AudioProcessing*)ec->params.webrtc.apm;
    ec->params.webrtc.apm = NULL;
  }
  
  for (i = 0; i < ec->params.webrtc.rec_ss.channels; i++)
    pa_xfree(ec->params.webrtc.rec_buffer[i]);
  for (i = 0; i < ec->params.webrtc.play_ss.channels; i++)
    pa_xfree(ec->params.webrtc.play_buffer[i]);
}