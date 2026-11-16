#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <opus/opus.h>
#include <speex/speex_opus.h>

#define PACKET_CAPACITY 4096
#define MAX_PACKETS 48
#define OUTPUT_RATE 48000
#define MAX_SAMPLES (SPEEX_OPUS_MAX_FRAME_SAMPLES)

static int failures;

#define CHECK(cond, ...) do { \
   if (!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__); \
      fprintf(stderr, "\n"); \
      failures++; \
   } \
} while (0)

static long long energy(const short *pcm, int samples)
{
   long long e = 0;
   int i;
   for (i = 0; i < samples; i++)
      e += pcm[i] < 0 ? -(long long)pcm[i] : pcm[i];
   return e;
}

static int output_samples(const SpeexOpusPacketInfo *info)
{
   return (int)((long long)info->packet_duration_us * OUTPUT_RATE /
                1000000LL);
}

static double now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

static int bandwidth_from_opus(int bandwidth)
{
   switch (bandwidth) {
   case OPUS_BANDWIDTH_NARROWBAND:
      return SPEEX_OPUS_BANDWIDTH_NARROWBAND;
   case OPUS_BANDWIDTH_MEDIUMBAND:
      return SPEEX_OPUS_BANDWIDTH_MEDIUMBAND;
   case OPUS_BANDWIDTH_WIDEBAND:
      return SPEEX_OPUS_BANDWIDTH_WIDEBAND;
   case OPUS_BANDWIDTH_SUPERWIDEBAND:
      return SPEEX_OPUS_BANDWIDTH_SUPERWIDEBAND;
   default:
      return SPEEX_OPUS_BANDWIDTH_FULLBAND;
   }
}

static int rate_from_bandwidth(int bandwidth)
{
   switch (bandwidth) {
   case SPEEX_OPUS_BANDWIDTH_NARROWBAND:
      return 8000;
   case SPEEX_OPUS_BANDWIDTH_MEDIUMBAND:
      return 12000;
   case SPEEX_OPUS_BANDWIDTH_WIDEBAND:
      return 16000;
   case SPEEX_OPUS_BANDWIDTH_SUPERWIDEBAND:
      return 24000;
   default:
      return 48000;
   }
}

int main(void)
{
   OpusEncoder *enc;
   int err = OPUS_OK;
   unsigned char packets[MAX_PACKETS][PACKET_CAPACITY];
   int lens[MAX_PACKETS];
   SpeexOpusPacketInfo infos[MAX_PACKETS];
   int durations[] = {120, 240, 480, 960, 1920, 2880};
   int bandwidths[] = {OPUS_BANDWIDTH_NARROWBAND, OPUS_BANDWIDTH_WIDEBAND,
                       OPUS_BANDWIDTH_FULLBAND};
   int i, j, total_expected = 0, total_decoded = 0;
   long long post_loss_energy = 0;
   SpeexOpusDecoder *dec;
   short pcm[MAX_SAMPLES];
   double decode_us = 0;
   int decoded_packets = 0;

   enc = opus_encoder_create(OUTPUT_RATE, 1, OPUS_APPLICATION_VOIP, &err);
   CHECK(enc != NULL && err == OPUS_OK, "encoder init failed: %d", err);
   if (!enc)
      return 1;
   opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));
   opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000));

   for (i = 0; i < MAX_PACKETS; i++) {
      int frame_samples = durations[i % 6];
      int bandwidth = bandwidths[(i / 6) % 3];
      short input[MAX_SAMPLES];
      int len;

      opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(bandwidth));
      for (j = 0; j < frame_samples; j++)
         input[j] = (short)(8000.0 * sin(2.0 * 3.141592653589793 *
                         440.0 * (i * 2880 + j) / OUTPUT_RATE));
      len = opus_encode(enc, input, frame_samples, packets[i],
                        PACKET_CAPACITY);
      CHECK(len > 0, "encode %d failed: %d", i, len);
      if (len <= 0)
         continue;
      lens[i] = len;
      CHECK(speex_opus_packet_parse(packets[i], len, &infos[i]) ==
            SPEEX_OPUS_OK, "parse %d failed", i);
      CHECK(infos[i].frames_per_packet ==
            opus_packet_get_nb_frames(packets[i], len),
            "frame count %d: %d != %d", i, infos[i].frames_per_packet,
            opus_packet_get_nb_frames(packets[i], len));
      CHECK(infos[i].sample_rate ==
            (bandwidth == OPUS_BANDWIDTH_NARROWBAND ? 8000 :
             bandwidth == OPUS_BANDWIDTH_WIDEBAND ? 16000 : 48000),
            "declared rate %d: %d", i, infos[i].sample_rate);
      CHECK(infos[i].frame_size ==
            opus_packet_get_samples_per_frame(packets[i],
                                              infos[i].sample_rate),
            "frame size %d: %d", i, infos[i].frame_size);
      CHECK(infos[i].packet_samples == (int)((long long)
            infos[i].packet_duration_us * infos[i].sample_rate / 1000000LL),
            "packet samples %d", i);
      total_expected += output_samples(&infos[i]);
   }
   opus_encoder_destroy(enc);

   dec = speex_opus_decoder_init(OUTPUT_RATE, 1);
   CHECK(dec != NULL, "decoder init failed");
   if (!dec)
      return 1;

   for (i = 0; i < MAX_PACKETS; i++) {
      SpeexOpusPacketInfo out_info;
      double t0 = now_us();
      int n;

      if (i >= 8 && i <= 12) {
         n = speex_opus_decode_loss(dec, infos[i].packet_duration_us, pcm,
                                    MAX_SAMPLES);
         CHECK(n == output_samples(&infos[i]),
               "loss %d length: %d", i, n);
         total_decoded += n;
         continue;
      }

      n = speex_opus_decode(dec, packets[i], lens[i], pcm, MAX_SAMPLES,
                            &out_info);
      decode_us += now_us() - t0;
      decoded_packets++;
      CHECK(n == output_samples(&infos[i]),
            "decode %d length: %d expected %d", i, n,
            output_samples(&infos[i]));
      CHECK(out_info.concealed == 0, "good packet %d marked concealed", i);
      CHECK(out_info.frame_duration_us == infos[i].frame_duration_us,
            "duration changed on decode %d", i);
      CHECK(out_info.sample_rate == infos[i].sample_rate,
            "rate changed on decode %d", i);
      total_decoded += n > 0 ? n : 0;
      if (i == 13)
         post_loss_energy = energy(pcm, n);
   }

   CHECK(total_decoded == total_expected,
         "total sample count: %d != %d", total_decoded, total_expected);
   CHECK(post_loss_energy > 1000,
         "audio after consecutive losses is silent (%lld)", post_loss_energy);
   CHECK(speex_opus_decoder_get_latency_us(dec) >= 6500,
         "latency not reported");
   {
      spx_int32_t lookahead = 0;
      spx_int32_t loss_streak = 0;
      CHECK(speex_opus_decoder_ctl(dec, SPEEX_OPUS_GET_LOOKAHEAD,
                                   &lookahead) == SPEEX_OPUS_OK,
            "lookahead ctl failed");
      CHECK(lookahead == 312, "unexpected 48 kHz lookahead: %d", lookahead);
      CHECK(speex_opus_decoder_ctl(dec, SPEEX_OPUS_GET_LOSS_STREAK,
                                   &loss_streak) == SPEEX_OPUS_OK &&
            loss_streak == 0, "loss streak did not reset");
   }

   /* A corrupt packet must conceal and leave the stream usable. */
   {
      unsigned char bad[] = {0x03};
      SpeexOpusPacketInfo bad_info;
      int n = speex_opus_decode(dec, bad, sizeof(bad), pcm, MAX_SAMPLES,
                                &bad_info);
      int good;
      CHECK(n > 0 && bad_info.concealed, "corrupt packet not concealed");
      good = speex_opus_decode(dec, packets[20], lens[20], pcm, MAX_SAMPLES,
                               NULL);
      CHECK(good > 0, "decoder stopped after corrupt packet");
   }

   /* Random short buffers must never break parsing or the receive loop. */
   {
      int iter;
      srand(1);
      for (iter = 0; iter < 500; iter++) {
         unsigned char junk[96];
         SpeexOpusPacketInfo junk_info;
         int len = 1 + rand() % (int)sizeof(junk);
         int k;
         for (k = 0; k < len; k++)
            junk[k] = (unsigned char)rand();
         if (speex_opus_packet_parse(junk, len, &junk_info) ==
             SPEEX_OPUS_OK) {
            CHECK(junk_info.frames_per_packet >= 1 &&
                  junk_info.frames_per_packet <= SPEEX_OPUS_MAX_FRAMES,
                  "junk frame count");
            CHECK(junk_info.packet_duration_us > 0 &&
                  junk_info.packet_duration_us <=
                  SPEEX_OPUS_MAX_PACKET_DURATION_US, "junk duration");
         }
         CHECK(speex_opus_decode(dec, junk, len, pcm, MAX_SAMPLES, NULL) > 0,
               "junk decode %d did not continue", iter);
      }
   }

   /* Verify every declared bandwidth/rate combination against libopus. */
   {
      int all_bandwidths[] = {OPUS_BANDWIDTH_NARROWBAND,
                              OPUS_BANDWIDTH_MEDIUMBAND,
                              OPUS_BANDWIDTH_WIDEBAND,
                              OPUS_BANDWIDTH_SUPERWIDEBAND,
                              OPUS_BANDWIDTH_FULLBAND};
      short input[960];
      int b;

      for (j = 0; j < 960; j++)
         input[j] = (short)(6000.0 * sin(2.0 * 3.141592653589793 *
                         300.0 * j / OUTPUT_RATE));
      enc = opus_encoder_create(OUTPUT_RATE, 1, OPUS_APPLICATION_VOIP, &err);
      CHECK(enc != NULL, "bandwidth encoder init failed");
      if (enc) {
         opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000));
         for (b = 0; b < 5; b++) {
            unsigned char pkt[PACKET_CAPACITY];
            SpeexOpusPacketInfo info;
            int len;
            opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(all_bandwidths[b]));
            len = opus_encode(enc, input, 960, pkt, PACKET_CAPACITY);
            CHECK(len > 0, "bandwidth encode %d failed", b);
            if (len <= 0)
               continue;
            CHECK(speex_opus_packet_parse(pkt, len, &info) == SPEEX_OPUS_OK,
                  "bandwidth parse %d failed", b);
            CHECK(info.bandwidth ==
                  bandwidth_from_opus(opus_packet_get_bandwidth(pkt)),
                  "bandwidth mapping %d", b);
            CHECK(info.sample_rate == rate_from_bandwidth(info.bandwidth),
                  "bandwidth rate %d: %d", b, info.sample_rate);
         }
         opus_encoder_destroy(enc);
      }
   }

   /* Sweep every TOC configuration, including the rates the encoder cannot
      be forced to produce directly. */
   {
      int config;
      for (config = 0; config < 32; config++) {
         unsigned char pkt[1];
         SpeexOpusPacketInfo info;
         int lib_bw;
         pkt[0] = (unsigned char)(config << 3);
         CHECK(speex_opus_packet_parse(pkt, 1, &info) == SPEEX_OPUS_OK,
               "config %d parse failed", config);
         lib_bw = opus_packet_get_bandwidth(pkt);
         CHECK(info.bandwidth == bandwidth_from_opus(lib_bw),
               "config %d bandwidth", config);
         CHECK(info.sample_rate == rate_from_bandwidth(info.bandwidth),
               "config %d rate", config);
         CHECK(info.frame_size ==
               opus_packet_get_samples_per_frame(pkt, info.sample_rate),
               "config %d frame size", config);
      }
   }

   /* The packet keeps its declared rate while the playback rate is fixed. */
   {
      SpeexOpusDecoder *dec8 = speex_opus_decoder_init(8000, 1);
      SpeexOpusPacketInfo info;
      int n;
      CHECK(dec8 != NULL, "8 kHz decoder init failed");
      if (dec8) {
         n = speex_opus_decode(dec8, packets[3], lens[3], pcm, MAX_SAMPLES,
                               &info);
         CHECK(n == infos[3].packet_duration_us * 8000 / 1000000,
               "8 kHz output length %d", n);
         CHECK(info.sample_rate == infos[3].sample_rate,
               "declared rate changed at 8 kHz output");
         speex_opus_decoder_destroy(dec8);
      }
   }

   /* Build a two-frame code-3 packet from one real frame and decode it. */
   {
      unsigned char multi[PACKET_CAPACITY];
      SpeexOpusPacketInfo info;
      int payload = lens[0] - 1;
      int n;
      if (payload < 252 && (packets[0][0] & 3) == 0) {
         multi[0] = (unsigned char)((packets[0][0] & 0xfc) | 3);
         multi[1] = 0x82;
         multi[2] = (unsigned char)payload;
         memcpy(multi + 3, packets[0] + 1, payload);
         memcpy(multi + 3 + payload, packets[0] + 1, payload);
         CHECK(speex_opus_packet_parse(multi, 3 + 2 * payload, &info) ==
               SPEEX_OPUS_OK, "multi-frame parse failed");
         CHECK(info.frames_per_packet == 2, "multi-frame count %d",
               info.frames_per_packet);
         n = speex_opus_decode(dec, multi, 3 + 2 * payload, pcm,
                               MAX_SAMPLES, &info);
         CHECK(n == 2 * output_samples(&infos[0]),
               "multi-frame decode length %d", n);
      }
   }

   /* Exercise the SpeexMode adapter, including its variable output length. */
   {
      void *mode_dec = speex_decoder_init(&speex_opus_mode);
      SpeexBits bits;
      spx_int32_t max_samples = 0, actual = 0;
      spx_int32_t mode_rate = OUTPUT_RATE;
      short mode_pcm[MAX_SAMPLES];
      CHECK(speex_lib_get_mode(SPEEX_MODEID_OPUS) == &speex_opus_mode,
            "opus mode not reachable");
      CHECK(mode_dec != NULL, "mode decoder init failed");
      if (mode_dec) {
         speex_decoder_ctl(mode_dec, SPEEX_SET_SAMPLING_RATE, &mode_rate);
         speex_decoder_ctl(mode_dec, SPEEX_GET_FRAME_SIZE, &max_samples);
         CHECK(max_samples == MAX_SAMPLES, "max frame size %d", max_samples);
         speex_bits_init(&bits);
         speex_bits_read_from(&bits, (const char *)packets[0], lens[0]);
         CHECK(speex_decode_int(mode_dec, &bits, mode_pcm) == 0,
               "mode decode failed");
         speex_decoder_ctl(mode_dec, SPEEX_OPUS_GET_LAST_FRAME_SIZE,
                           &actual);
         CHECK(actual == output_samples(&infos[0]),
               "mode actual frame size %d", actual);
         speex_bits_destroy(&bits);
         speex_decoder_destroy(mode_dec);
      }
   }

   printf("opus adapter: packets=%d samples=%d lookahead=6.5ms "
          "latency_20ms=%dus avg_decode=%.1fus\n",
          MAX_PACKETS, total_decoded,
          6500 + 20000,
          decoded_packets ? decode_us / decoded_packets : 0.0);
   speex_opus_decoder_destroy(dec);

   if (failures) {
      fprintf(stderr, "%d opus adapter checks failed\n", failures);
      return 1;
   }
   return 0;
}
