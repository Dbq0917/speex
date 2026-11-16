#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "speex/speex_opus.h"
#include "arch.h"
#include "os_support.h"

#include <string.h>

#ifdef HAVE_OPUS
#include <opus/opus.h>
#endif

#ifndef EXPORT
#define EXPORT
#endif

#define OPUS_US_PER_MS 1000
#define OPUS_DEFAULT_FRAME_US 20000

static const int opus_bandwidth_rate[5] = {8000, 12000, 16000, 24000, 48000};

static int opus_valid_output_rate(int rate)
{
   return rate == 8000 || rate == 12000 || rate == 16000 ||
          rate == 24000 || rate == 48000;
}

static int opus_duration_us(const unsigned char *packet)
{
   int config = packet[0] >> 3;

   if (config < 4)
      return (config == 3 ? 60 : (10 << config)) * OPUS_US_PER_MS;
   if (config < 8)
      return ((config & 3) == 3 ? 60 : (10 << (config & 3))) * OPUS_US_PER_MS;
   if (config < 12)
      return ((config & 3) == 3 ? 60 : (10 << (config & 3))) * OPUS_US_PER_MS;
   if (config < 14)
      return (config & 1 ? 20 : 10) * OPUS_US_PER_MS;
   if (config < 16)
      return (config & 1 ? 20 : 10) * OPUS_US_PER_MS;
   return (2500 << ((config - 16) & 3));
}

static int opus_bandwidth(const unsigned char *packet)
{
   int config = packet[0] >> 3;

   if (config < 12)
      return config >> 2;
   if (config < 14)
      return SPEEX_OPUS_BANDWIDTH_SUPERWIDEBAND;
   if (config < 16)
      return SPEEX_OPUS_BANDWIDTH_FULLBAND;
   if (config < 20)
      return SPEEX_OPUS_BANDWIDTH_NARROWBAND;
   if (config < 24)
      return SPEEX_OPUS_BANDWIDTH_WIDEBAND;
   if (config < 28)
      return SPEEX_OPUS_BANDWIDTH_SUPERWIDEBAND;
   return SPEEX_OPUS_BANDWIDTH_FULLBAND;
}

static int opus_parse_size(const unsigned char *data, int len, int *size)
{
   if (len < 1)
      return -1;
   if (data[0] < 252) {
      *size = data[0];
      return 1;
   }
   if (len < 2)
      return -1;
   *size = 4 * data[1] + data[0];
   return 2;
}

static int opus_packet_decode_info(const unsigned char *packet, int len,
                                   SpeexOpusPacketInfo *info)
{
   int toc, code, count, frame_us, bandwidth, payload_offset = 1;
   int remaining, last_size, i;
   int sizes[SPEEX_OPUS_MAX_FRAMES];

   if (!packet || !info || len <= 0)
      return SPEEX_OPUS_ERR_BAD_PACKET;

   memset(info, 0, sizeof(*info));
   toc = packet[0];
   code = toc & 3;
   frame_us = opus_duration_us(packet);
   bandwidth = opus_bandwidth(packet);

   remaining = len - 1;
   last_size = remaining;

   switch (code) {
   case 0:
      count = 1;
      break;
   case 1:
      count = 2;
      if (remaining & 1)
         return SPEEX_OPUS_ERR_BAD_PACKET;
      last_size = remaining / 2;
      sizes[0] = last_size;
      break;
   case 2: {
      int bytes = opus_parse_size(packet + payload_offset, remaining, &sizes[0]);
      if (bytes < 0)
         return SPEEX_OPUS_ERR_BAD_PACKET;
      remaining -= bytes;
      payload_offset += bytes;
      if (sizes[0] < 0 || sizes[0] > remaining)
         return SPEEX_OPUS_ERR_BAD_PACKET;
      last_size = remaining - sizes[0];
      count = 2;
      break;
   }
   default: {
      int ch, cbr, pad = 0;
      if (remaining < 1)
         return SPEEX_OPUS_ERR_BAD_PACKET;
      ch = packet[payload_offset++];
      remaining--;
      count = ch & 0x3f;
      if (count <= 0 || frame_us * count > SPEEX_OPUS_MAX_PACKET_DURATION_US)
         return SPEEX_OPUS_ERR_BAD_PACKET;

      if (ch & 0x40) {
         int p;
         do {
            int tmp;
            if (remaining <= 0)
               return SPEEX_OPUS_ERR_BAD_PACKET;
            p = packet[payload_offset++];
            remaining--;
            tmp = p == 255 ? 254 : p;
            remaining -= tmp;
            pad += tmp;
         } while (p == 255);
      }
      if (remaining < 0)
         return SPEEX_OPUS_ERR_BAD_PACKET;

      cbr = !(ch & 0x80);
      if (!cbr) {
         last_size = remaining;
         for (i = 0; i < count - 1; i++) {
            int bytes = opus_parse_size(packet + payload_offset, remaining,
                                        &sizes[i]);
            if (bytes < 0)
               return SPEEX_OPUS_ERR_BAD_PACKET;
            remaining -= bytes;
            payload_offset += bytes;
            if (sizes[i] < 0 || sizes[i] > remaining)
               return SPEEX_OPUS_ERR_BAD_PACKET;
            last_size -= bytes + sizes[i];
         }
         if (last_size < 0)
            return SPEEX_OPUS_ERR_BAD_PACKET;
      } else {
         last_size = remaining / count;
         if (last_size * count != remaining)
            return SPEEX_OPUS_ERR_BAD_PACKET;
         for (i = 0; i < count - 1; i++)
            sizes[i] = last_size;
      }
      (void)pad;
      break;
   }
   }

   if (last_size > 1275)
      return SPEEX_OPUS_ERR_BAD_PACKET;
   sizes[count - 1] = last_size;
   for (i = 0; i < count; i++) {
      if (sizes[i] < 0 || sizes[i] > remaining)
         return SPEEX_OPUS_ERR_BAD_PACKET;
      remaining -= sizes[i];
   }

   info->bandwidth = bandwidth;
   info->sample_rate = opus_bandwidth_rate[bandwidth];
   info->channels = (toc & 0x4) ? 2 : 1;
   info->frame_duration_us = frame_us;
   info->frames_per_packet = count;
   info->packet_duration_us = frame_us * count;
   info->frame_size = (int)((long long)info->packet_duration_us *
                            info->sample_rate / (1000000LL * count));
   info->packet_samples = (int)((long long)info->packet_duration_us *
                                info->sample_rate / 1000000LL);
   info->toc = toc;
   info->payload_offset = payload_offset;
   info->packet_bytes = len;
   info->concealed = 0;
   return SPEEX_OPUS_OK;
}

EXPORT int speex_opus_packet_parse(const void *packet, int len,
                                   SpeexOpusPacketInfo *info)
{
   return opus_packet_decode_info((const unsigned char *)packet, len, info);
}

struct SpeexOpusDecoder {
   const SpeexMode *mode;
#ifdef HAVE_OPUS
   OpusDecoder *opus;
#endif
   int output_rate;
   int channels;
   int lookahead;
   int max_output_samples;
   int loss_streak;
   int last_error;
   int activity;
   int have_last;
   SpeexOpusPacketInfo last_info;
};

static int opus_output_samples(const SpeexOpusDecoder *st,
                               const SpeexOpusPacketInfo *info)
{
   return (int)((long long)info->packet_duration_us * st->output_rate /
                1000000LL);
}

static void opus_set_default_info(SpeexOpusDecoder *st, SpeexOpusPacketInfo *info)
{
   memset(info, 0, sizeof(*info));
   info->sample_rate = st->output_rate;
   info->channels = st->channels;
   info->frame_duration_us = OPUS_DEFAULT_FRAME_US;
   info->frames_per_packet = 1;
   info->packet_duration_us = OPUS_DEFAULT_FRAME_US;
   info->frame_size = (int)((long long)OPUS_DEFAULT_FRAME_US *
                            st->output_rate / 1000000LL);
   info->packet_samples = info->frame_size;
   info->bandwidth = st->output_rate <= 8000 ? SPEEX_OPUS_BANDWIDTH_NARROWBAND :
                     st->output_rate <= 12000 ? SPEEX_OPUS_BANDWIDTH_MEDIUMBAND :
                     st->output_rate <= 16000 ? SPEEX_OPUS_BANDWIDTH_WIDEBAND :
                     st->output_rate <= 24000 ? SPEEX_OPUS_BANDWIDTH_SUPERWIDEBAND :
                     SPEEX_OPUS_BANDWIDTH_FULLBAND;
}

EXPORT SpeexOpusDecoder *speex_opus_decoder_init(int output_rate, int channels)
{
   SpeexOpusDecoder *st;

   if (output_rate == 0)
      output_rate = 48000;
   if (!opus_valid_output_rate(output_rate) || (channels != 1 && channels != 2))
      return NULL;

   st = (SpeexOpusDecoder *)speex_alloc(sizeof(*st));
   if (!st)
      return NULL;
   st->mode = &speex_opus_mode;
   st->output_rate = output_rate;
   st->channels = channels;
   st->last_error = SPEEX_OPUS_OK;
   st->max_output_samples = SPEEX_OPUS_MAX_FRAME_SAMPLES * output_rate / 48000;
   opus_set_default_info(st, &st->last_info);

#ifdef HAVE_OPUS
   {
      int err = OPUS_OK;
      st->opus = opus_decoder_create(output_rate, channels, &err);
      if (!st->opus || err != OPUS_OK) {
         if (st->opus)
            opus_decoder_destroy(st->opus);
         speex_free(st);
         return NULL;
      }
      st->lookahead = output_rate * 6500 / 1000000;
      {
         opus_int32 reported = 0;
         if (opus_decoder_ctl(st->opus, OPUS_GET_LOOKAHEAD(&reported)) ==
             OPUS_OK && reported > 0)
            st->lookahead = reported;
      }
   }
#else
   (void)st->lookahead;
   speex_free(st);
   return NULL;
#endif
   return st;
}

EXPORT SpeexOpusDecoder *speex_opus_decoder_create(int output_rate, int channels)
{
   return speex_opus_decoder_init(output_rate, channels);
}

EXPORT int speex_opus_decoder_configure(SpeexOpusDecoder *st, int output_rate,
                                        int channels)
{
   SpeexOpusDecoder *fresh;

   if (!st || !opus_valid_output_rate(output_rate) ||
       (channels != 1 && channels != 2))
      return SPEEX_OPUS_ERR_BAD_ARG;
   if (st->output_rate == output_rate && st->channels == channels)
      return SPEEX_OPUS_OK;

   fresh = speex_opus_decoder_init(output_rate, channels);
   if (!fresh)
      return SPEEX_OPUS_ERR_INTERNAL;
#ifdef HAVE_OPUS
   if (st->opus)
      opus_decoder_destroy(st->opus);
   st->opus = fresh->opus;
   fresh->opus = NULL;
#endif
   st->output_rate = fresh->output_rate;
   st->channels = fresh->channels;
   st->lookahead = fresh->lookahead;
   st->max_output_samples = fresh->max_output_samples;
   st->have_last = 0;
   st->loss_streak = 0;
   st->activity = 0;
   st->last_error = SPEEX_OPUS_OK;
   opus_set_default_info(st, &st->last_info);
   speex_opus_decoder_destroy(fresh);
   return SPEEX_OPUS_OK;
}

EXPORT void speex_opus_decoder_destroy(SpeexOpusDecoder *st)
{
   if (!st)
      return;
#ifdef HAVE_OPUS
   if (st->opus)
      opus_decoder_destroy(st->opus);
#endif
   speex_free(st);
}

EXPORT void speex_opus_decoder_free(SpeexOpusDecoder *st)
{
   speex_opus_decoder_destroy(st);
}

static int opus_update_activity(SpeexOpusDecoder *st, const spx_int16_t *pcm,
                                int samples)
{
   spx_int32_t energy = 0;
   int i, n = samples * st->channels;

   for (i = 0; i < n; i++) {
      spx_int32_t v = pcm[i];
      energy += v < 0 ? -v : v;
   }
   st->activity = (n > 0 && energy / n >= 30) ? 100 : 0;
   return st->activity;
}

static int opus_conceal(SpeexOpusDecoder *st, int duration_us,
                        spx_int16_t *pcm, int max_samples)
{
   int samples;
   int ret;

   if (!st || !pcm || max_samples <= 0)
      return SPEEX_OPUS_ERR_BAD_ARG;
   if (duration_us <= 0)
      duration_us = st->have_last ? st->last_info.packet_duration_us :
                                    OPUS_DEFAULT_FRAME_US;
   if (duration_us > SPEEX_OPUS_MAX_PACKET_DURATION_US)
      duration_us = SPEEX_OPUS_MAX_PACKET_DURATION_US;
   duration_us = (duration_us + 1250) / 2500 * 2500;
   if (duration_us < 2500)
      duration_us = 2500;
   samples = (int)((long long)duration_us * st->output_rate / 1000000LL);
   if (samples <= 0)
      samples = st->output_rate / 50;
   if (samples > max_samples)
      return SPEEX_OPUS_ERR_BUFFER_TOO_SMALL;

#ifdef HAVE_OPUS
   ret = opus_decode(st->opus, NULL, 0, pcm, samples, 0);
   if (ret < 0) {
      memset(pcm, 0, (size_t)samples * st->channels * sizeof(*pcm));
      ret = samples;
   }
#else
   memset(pcm, 0, (size_t)samples * st->channels * sizeof(*pcm));
   ret = samples;
#endif

   st->loss_streak++;
   st->last_error = SPEEX_OPUS_OK;
   st->activity = 0;
   st->last_info.concealed = 1;
   st->last_info.packet_duration_us = duration_us;
   st->last_info.packet_samples = (int)((long long)duration_us *
                                        st->last_info.sample_rate / 1000000LL);
   st->last_info.packet_bytes = 0;
   st->last_info.payload_offset = 0;
   if (st->last_info.frame_duration_us > 0)
      st->last_info.frames_per_packet = duration_us /
                                        st->last_info.frame_duration_us;
   return ret;
}

EXPORT int speex_opus_decode_loss(SpeexOpusDecoder *st, int lost_duration_us,
                                  spx_int16_t *pcm, int max_samples)
{
   return opus_conceal(st, lost_duration_us, pcm, max_samples);
}

EXPORT int speex_opus_decode(SpeexOpusDecoder *st, const void *packet, int len,
                             spx_int16_t *pcm, int max_samples,
                             SpeexOpusPacketInfo *info)
{
   SpeexOpusPacketInfo parsed;
   int samples, ret;

   if (!st || !pcm || max_samples <= 0)
      return SPEEX_OPUS_ERR_BAD_ARG;

   if (!packet || len <= 0) {
      ret = opus_conceal(st, 0, pcm, max_samples);
      if (info)
         *info = st->last_info;
      return ret;
   }

   if (opus_packet_decode_info((const unsigned char *)packet, len, &parsed) !=
       SPEEX_OPUS_OK) {
      ret = opus_conceal(st, st->have_last ? st->last_info.packet_duration_us :
                                             OPUS_DEFAULT_FRAME_US,
                         pcm, max_samples);
      if (ret > 0) {
         st->last_error = SPEEX_OPUS_ERR_BAD_PACKET;
         st->last_info.concealed = 1;
      }
      if (info)
         *info = st->last_info;
      return ret;
   }

   samples = opus_output_samples(st, &parsed);
   if (samples <= 0 || samples > max_samples)
      return SPEEX_OPUS_ERR_BUFFER_TOO_SMALL;

#ifdef HAVE_OPUS
   ret = opus_decode(st->opus, (const unsigned char *)packet, len, pcm,
                     samples, 0);
   if (ret < 0) {
      ret = opus_conceal(st, parsed.packet_duration_us, pcm, max_samples);
      st->last_error = SPEEX_OPUS_ERR_BAD_PACKET;
      st->last_info.concealed = 1;
      if (info)
         *info = st->last_info;
      return ret;
   }
#else
   memset(pcm, 0, (size_t)samples * st->channels * sizeof(*pcm));
   ret = samples;
#endif

   st->loss_streak = 0;
   st->last_error = SPEEX_OPUS_OK;
   st->have_last = 1;
   st->last_info = parsed;
   st->last_info.concealed = 0;
   opus_update_activity(st, pcm, ret);
   if (info)
      *info = st->last_info;
   return ret;
}

EXPORT int speex_opus_decode_packet(SpeexOpusDecoder *st, const void *packet,
                                    int len, spx_int16_t *pcm,
                                    int max_samples, SpeexOpusPacketInfo *info)
{
   return speex_opus_decode(st, packet, len, pcm, max_samples, info);
}

EXPORT int speex_opus_decoder_get_info(SpeexOpusDecoder *st,
                                       SpeexOpusPacketInfo *info)
{
   if (!st || !info)
      return SPEEX_OPUS_ERR_BAD_ARG;
   *info = st->last_info;
   return SPEEX_OPUS_OK;
}

static void *opus_mode_decoder_init(const SpeexMode *mode)
{
   (void)mode;
   return speex_opus_decoder_init(48000, 1);
}

static void opus_mode_decoder_destroy(void *state)
{
   speex_opus_decoder_destroy((SpeexOpusDecoder *)state);
}

static int opus_mode_query(const void *mode, int request, void *ptr)
{
   (void)mode;
   switch (request) {
   case SPEEX_MODE_FRAME_SIZE:
      *(int *)ptr = SPEEX_OPUS_MAX_FRAME_SAMPLES;
      return 0;
   case SPEEX_SUBMODE_BITS_PER_FRAME:
      *(int *)ptr = -1;
      return 0;
   default:
      return -1;
   }
}

static int opus_mode_decode(void *state, SpeexBits *bits, void *out)
{
   SpeexOpusDecoder *st = (SpeexOpusDecoder *)state;
   int len, ret, max_samples, byte_offset;
   const unsigned char *data;
#ifndef FIXED_POINT
   spx_int16_t *tmp = NULL;
   spx_word16_t *dst = (spx_word16_t *)out;
   int i;
#endif

   if (!st)
      return -2;
   if (!bits) {
#ifdef FIXED_POINT
      ret = opus_conceal(st, 0, (spx_int16_t *)out,
                         st->max_output_samples);
#else
      tmp = (spx_int16_t *)speex_alloc(st->max_output_samples *
                                       st->channels * sizeof(*tmp));
      if (!tmp)
         return -1;
      ret = opus_conceal(st, 0, tmp, st->max_output_samples);
      if (ret > 0) {
         for (i = 0; i < ret * st->channels; i++)
            ((spx_word16_t *)out)[i] = (spx_word16_t)tmp[i];
      }
      speex_free(tmp);
#endif
      return ret < 0 ? -1 : 0;
   }
   if (bits->bitPtr != 0)
      return -2;
   byte_offset = bits->charPtr * BYTES_PER_CHAR;
   len = (bits->nbBits >> 3) - byte_offset;
   if (len <= 0)
      return -1;
   data = (const unsigned char *)bits->chars + byte_offset;
   max_samples = st->max_output_samples;

#ifdef FIXED_POINT
   ret = speex_opus_decode(st, data, len, (spx_int16_t *)out, max_samples,
                           NULL);
#else
   tmp = (spx_int16_t *)speex_alloc(max_samples * st->channels *
                                    sizeof(*tmp));
   if (!tmp)
      return -1;
   ret = speex_opus_decode(st, data, len, tmp, max_samples, NULL);
   if (ret > 0) {
      for (i = 0; i < ret * st->channels; i++)
         dst[i] = (spx_word16_t)tmp[i];
   }
   speex_free(tmp);
#endif
   if (ret < 0)
      return -2;
   bits->charPtr += len / BYTES_PER_CHAR;
   return 0;
}

EXPORT int speex_opus_decoder_ctl(SpeexOpusDecoder *st, int request, void *ptr)
{
   if (!st)
      return SPEEX_OPUS_ERR_BAD_ARG;
   switch (request) {
   case SPEEX_SET_ENH:
   case SPEEX_SET_PLC_TUNING:
      return SPEEX_OPUS_OK;
   case SPEEX_GET_ENH:
      *(spx_int32_t *)ptr = 1;
      return SPEEX_OPUS_OK;
   case SPEEX_GET_PLC_TUNING:
      *(spx_int32_t *)ptr = 2;
      return SPEEX_OPUS_OK;
   case SPEEX_GET_FRAME_SIZE:
      *(spx_int32_t *)ptr = st->max_output_samples;
      return SPEEX_OPUS_OK;
   case SPEEX_SET_SAMPLING_RATE:
      return speex_opus_decoder_configure(st, *(spx_int32_t *)ptr,
                                          st->channels);
   case SPEEX_GET_SAMPLING_RATE:
   case SPEEX_OPUS_GET_OUTPUT_RATE:
      *(spx_int32_t *)ptr = st->output_rate;
      return SPEEX_OPUS_OK;
   case SPEEX_GET_LOOKAHEAD:
   case SPEEX_OPUS_GET_LOOKAHEAD:
      *(spx_int32_t *)ptr = st->lookahead;
      return SPEEX_OPUS_OK;
   case SPEEX_GET_ACTIVITY:
      *(spx_int32_t *)ptr = st->activity;
      return SPEEX_OPUS_OK;
   case SPEEX_GET_BITRATE:
      if (st->last_info.packet_duration_us > 0)
         *(spx_int32_t *)ptr = (spx_int32_t)
            ((long long)st->last_info.packet_bytes * 8000000 /
             st->last_info.packet_duration_us);
      else
         *(spx_int32_t *)ptr = 0;
      return SPEEX_OPUS_OK;
   case SPEEX_RESET_STATE:
#ifdef HAVE_OPUS
      opus_decoder_ctl(st->opus, OPUS_RESET_STATE);
#endif
      st->loss_streak = 0;
      st->last_error = SPEEX_OPUS_OK;
      st->activity = 0;
      st->have_last = 0;
      opus_set_default_info(st, &st->last_info);
      return SPEEX_OPUS_OK;
   case SPEEX_OPUS_GET_PACKET_INFO:
      *(SpeexOpusPacketInfo *)ptr = st->last_info;
      return SPEEX_OPUS_OK;
   case SPEEX_OPUS_GET_DECLARED_RATE:
      *(spx_int32_t *)ptr = st->last_info.sample_rate;
      return SPEEX_OPUS_OK;
   case SPEEX_OPUS_GET_LAST_FRAME_SIZE:
      *(spx_int32_t *)ptr = (spx_int32_t)((long long)
                              st->last_info.packet_duration_us *
                              st->output_rate / 1000000LL);
      return SPEEX_OPUS_OK;
   case SPEEX_OPUS_GET_LOSS_STREAK:
      *(spx_int32_t *)ptr = st->loss_streak;
      return SPEEX_OPUS_OK;
   case SPEEX_OPUS_GET_ERROR:
      *(spx_int32_t *)ptr = st->last_error;
      return SPEEX_OPUS_OK;
   default:
      return -1;
   }
}

EXPORT int speex_opus_decoder_get_latency_us(SpeexOpusDecoder *st)
{
   if (!st)
      return SPEEX_OPUS_ERR_BAD_ARG;
   return (int)((long long)st->lookahead * 1000000 / st->output_rate +
          (st->have_last ? st->last_info.packet_duration_us :
                           OPUS_DEFAULT_FRAME_US));
}

static int opus_mode_decoder_ctl(void *state, int request, void *ptr)
{
   return speex_opus_decoder_ctl((SpeexOpusDecoder *)state, request, ptr);
}

static void *opus_mode_encoder_init(const SpeexMode *mode)
{
   (void)mode;
   return 0;
}

static void opus_mode_encoder_destroy(void *state)
{
   (void)state;
}

static int opus_mode_encode(void *state, void *in, SpeexBits *bits)
{
   (void)state;
   (void)in;
   (void)bits;
   return -1;
}

static int opus_mode_encoder_ctl(void *state, int request, void *ptr)
{
   (void)state;
   (void)request;
   (void)ptr;
   return -1;
}

EXPORT const SpeexMode speex_opus_mode = {
   NULL,
   opus_mode_query,
   "opus",
   SPEEX_MODEID_OPUS,
   0,
   opus_mode_encoder_init,
   opus_mode_encoder_destroy,
   opus_mode_encode,
   opus_mode_decoder_init,
   opus_mode_decoder_destroy,
   opus_mode_decode,
   opus_mode_encoder_ctl,
   opus_mode_decoder_ctl,
};
