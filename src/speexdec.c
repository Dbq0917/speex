/* Copyright (C) 2002-2006 Jean-Marc Valin
   File: speexdec.c

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#if !defined WIN32 && !defined _WIN32
#include <unistd.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#ifndef HAVE_GETOPT_LONG
#include "getopt_win.h"
#endif
#include <stdlib.h>
#include <string.h>

#include <ogg/ogg.h>

#if defined WIN32 || defined _WIN32
#include "wave_out.h"
/* We need the following two to set stdout to binary */
#include <io.h>
#include <fcntl.h>
#endif
#include <math.h>

#ifdef __MINGW32__
#include "wave_out.c"
#endif

#ifdef HAVE_SYS_SOUNDCARD_H
#include <sys/soundcard.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#elif defined HAVE_SYS_AUDIOIO_H
#include <sys/types.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/audioio.h>
#ifndef AUDIO_ENCODING_SLINEAR
#define AUDIO_ENCODING_SLINEAR AUDIO_ENCODING_LINEAR /* Solaris */
#endif

#endif

#include <string.h>
#include "wav_io.h"
#include "speex/speex_header.h"
#include "speex/speex_stereo.h"
#include "speex/speex_callbacks.h"
#include "speex/speex_opus.h"

#define MAX_FRAME_SIZE 2000

#define readint(buf) ((((unsigned)buf[3]<<24)&0xff000000)| \
                      (((unsigned)buf[2]<<16)&0xff0000)| \
                      (((unsigned)buf[1]<<8)&0xff00)| \
                       ((unsigned)buf[0]&0xff))

static void print_comments(char *comments, int length)
{
   char *c=comments;
   int len, i, nb_fields;
   char *end;

   if (length<8)
   {
      fprintf (stderr, "Invalid/corrupted comments\n");
      return;
   }
   end = c+length;
   len=readint(c);
   c+=4;
   if (len < 0 || c+len>end)
   {
      fprintf (stderr, "Invalid/corrupted comments\n");
      return;
   }
   fwrite(c, 1, len, stderr);
   c+=len;
   fprintf (stderr, "\n");
   if (c+4>end)
   {
      fprintf (stderr, "Invalid/corrupted comments\n");
      return;
   }
   nb_fields=readint(c);
   c+=4;
   for (i=0;i<nb_fields;i++)
   {
      if (c+4>end)
      {
         fprintf (stderr, "Invalid/corrupted comments\n");
         return;
      }
      len=readint(c);
      c+=4;
      if (len < 0 || c+len>end)
      {
         fprintf (stderr, "Invalid/corrupted comments\n");
         return;
      }
      fwrite(c, 1, len, stderr);
      c+=len;
      fprintf (stderr, "\n");
   }
}

FILE *out_file_open(char *outFile, int rate, int *channels)
{
   FILE *fout=NULL;
   /*Open output file*/
   if (strlen(outFile)==0)
   {
#if defined HAVE_SYS_SOUNDCARD_H
      int audio_fd, format, stereo;
      audio_fd=open("/dev/dsp", O_WRONLY);
      if (audio_fd<0)
      {
         perror("Cannot open /dev/dsp");
         exit(1);
      }

      format=AFMT_S16_NE;
      if (ioctl(audio_fd, SNDCTL_DSP_SETFMT, &format)==-1)
      {
         perror("SNDCTL_DSP_SETFMT");
         close(audio_fd);
         exit(1);
      }

      stereo=0;
      if (*channels==2)
         stereo=1;
      if (ioctl(audio_fd, SNDCTL_DSP_STEREO, &stereo)==-1)
      {
         perror("SNDCTL_DSP_STEREO");
         close(audio_fd);
         exit(1);
      }
      if (stereo!=0)
      {
         if (*channels==1)
            fprintf (stderr, "Cannot set mono mode, will decode in stereo\n");
         *channels=2;
      }

      if (ioctl(audio_fd, SNDCTL_DSP_SPEED, &rate)==-1)
      {
         perror("SNDCTL_DSP_SPEED");
         close(audio_fd);
         exit(1);
      }
      fout = fdopen(audio_fd, "w");
#elif defined HAVE_SYS_AUDIOIO_H
      audio_info_t info;
      int audio_fd;

      audio_fd = open("/dev/audio", O_WRONLY);
      if (audio_fd<0)
      {
         perror("Cannot open /dev/audio");
         exit(1);
      }

      AUDIO_INITINFO(&info);
#ifdef AUMODE_PLAY    /* NetBSD/OpenBSD */
      info.mode = AUMODE_PLAY;
#endif
      info.play.encoding = AUDIO_ENCODING_SLINEAR;
      info.play.precision = 16;
      info.play.sample_rate = rate;
      info.play.channels = *channels;

      if (ioctl(audio_fd, AUDIO_SETINFO, &info) < 0)
      {
         perror ("AUDIO_SETINFO");
         exit(1);
      }
      fout = fdopen(audio_fd, "w");
#elif defined WIN32 || defined _WIN32
      {
         unsigned int speex_channels = *channels;
         if (Set_WIN_Params (INVALID_FILEDESC, rate, SAMPLE_SIZE, speex_channels))
         {
            fprintf (stderr, "Can't access %s\n", "WAVE OUT");
            exit(1);
         }
      }
#else
      fprintf (stderr, "No soundcard support\n");
      exit(1);
#endif
   } else {
      if (strcmp(outFile,"-")==0)
      {
#if defined WIN32 || defined _WIN32
         _setmode(_fileno(stdout), _O_BINARY);
#elif defined OS2
         _fsetmode(stdout,"b");
#endif
         fout=stdout;
      }
      else
      {
         fout = fopen(outFile, "wb");
         if (!fout)
         {
            perror(outFile);
            exit(1);
         }
         if (strcmp(outFile+strlen(outFile)-4,".wav")==0 || strcmp(outFile+strlen(outFile)-4,".WAV")==0)
            write_wav_header(fout, rate, *channels, 0, 0);
      }
   }
   return fout;
}

static int opus_le16(const unsigned char *p)
{
   return (int)p[0] | ((int)p[1] << 8);
}

static void opus_emit_pcm(FILE *fout, char *outFile, short *pcm,
                          short *le_pcm, int samples, int channels,
                          int *audio_size)
{
   int i;
   if (samples <= 0)
      return;
   for (i = 0; i < samples * channels; i++)
      le_pcm[i] = le_short(pcm[i]);
#if defined WIN32 || defined _WIN32
   if (strlen(outFile) == 0)
      WIN_Play_Samples(le_pcm, sizeof(short) * samples * channels);
   else
#endif
      fwrite(le_pcm, sizeof(short), (size_t)samples * channels, fout);
   *audio_size += (int)sizeof(short) * samples * channels;
}

static int decode_opus_ogg(FILE *fin, char *outFile, int wav_format,
                           float loss_percent, int quiet, int opus_info,
                           int forced_rate, int forced_channels)
{
   ogg_sync_state oy;
   ogg_stream_state os;
   ogg_page og;
   ogg_packet op;
   SpeexOpusDecoder *dec = NULL;
   SpeexOpusPacketInfo last_info;
   FILE *fout = NULL;
   short *pcm = NULL;
   short *le_pcm = NULL;
   int header_seen = 0, stream_init = 0, eos = 0;
   int channels = 1, rate, pre_skip = 0, audio_size = 0;
   int trim = 0;
   long long produced = 0;
   int result = 1;

   memset(&last_info, 0, sizeof(last_info));
   rate = forced_rate ? forced_rate : 48000;
   if (rate != 8000 && rate != 12000 && rate != 16000 &&
       rate != 24000 && rate != 48000) {
      fprintf(stderr, "Opus output rate must be 8000, 12000, 16000, 24000 or 48000 Hz\n");
      return 1;
   }

   ogg_sync_init(&oy);
   while (!eos) {
      char *data = ogg_sync_buffer(&oy, 4096);
      int nb_read = (int)fread(data, 1, 4096, fin);
      if (nb_read < 0)
         break;
      ogg_sync_wrote(&oy, nb_read);

      while (ogg_sync_pageout(&oy, &og) == 1) {
         if (!stream_init) {
            ogg_stream_init(&os, ogg_page_serialno(&og));
            stream_init = 1;
         } else if (ogg_page_serialno(&og) != os.serialno) {
            continue;
         }
         ogg_stream_pagein(&os, &og);

         while (ogg_stream_packetout(&os, &op) == 1) {
            if (!header_seen) {
               if (op.bytes < 19 || memcmp(op.packet, "OpusHead", 8) != 0) {
                  result = 2;
                  goto done;
               }
               if ((op.packet[8] >> 4) != 0) {
                  fprintf(stderr, "Unsupported Opus version %d\n", op.packet[8]);
                  goto done;
               }
               channels = op.packet[9];
               if (channels != 1 && channels != 2) {
                  fprintf(stderr, "Unsupported Opus channel count %d\n", channels);
                  goto done;
               }
               if (forced_channels == 1 || forced_channels == 2) {
                  if (forced_channels != channels && !quiet)
                     fprintf(stderr, "Downmixing Opus %d channels to %d\n",
                             channels, forced_channels);
                  channels = forced_channels;
               }
               if (op.packet[18] != 0) {
                  fprintf(stderr, "Unsupported Opus mapping family %d\n",
                          op.packet[18]);
                  goto done;
               }
               pre_skip = opus_le16((const unsigned char *)op.packet + 10);
               trim = (int)((long long)pre_skip * rate / 48000);
               dec = speex_opus_decoder_init(rate, channels);
               if (!dec) {
                  fprintf(stderr, "Opus decoder initialization failed\n");
                  goto done;
               }
               fout = out_file_open(outFile, rate, &channels);
               pcm = (short *)malloc((size_t)SPEEX_OPUS_MAX_FRAME_SAMPLES *
                                     channels * sizeof(*pcm));
               le_pcm = (short *)malloc((size_t)SPEEX_OPUS_MAX_FRAME_SAMPLES *
                                        channels * sizeof(*le_pcm));
               if (!pcm || !le_pcm) {
                  fprintf(stderr, "Out of memory\n");
                  goto done;
               }
               header_seen = 1;
               if (!quiet)
                  fprintf(stderr, "Decoding Opus %d Hz, %d channel%s\n",
                          rate, channels, channels == 1 ? "" : "s");
               continue;
            }

            if (op.bytes >= 8 && !memcmp(op.packet, "OpusTags", 8))
               continue;

            {
               SpeexOpusPacketInfo info;
               int n;
               int lost = loss_percent > 0 &&
                          100.0 * rand() / RAND_MAX < loss_percent;
               long long granule = op.granulepos >= 0 ? op.granulepos : -1;
               long long target_end = -1;

               if (granule >= 0)
                  target_end = ((long long)granule - pre_skip) * rate / 48000;
               if (lost) {
                  n = speex_opus_decode_loss(dec,
                           last_info.packet_duration_us ? last_info.packet_duration_us :
                           20000, pcm,
                           SPEEX_OPUS_MAX_FRAME_SAMPLES);
                  if (n < 0)
                     goto done;
               } else {
                  n = speex_opus_decode(dec, op.packet, op.bytes, pcm,
                                        SPEEX_OPUS_MAX_FRAME_SAMPLES, &info);
                  if (n < 0) {
                     fprintf(stderr, "Opus decode error %d\n", n);
                     goto done;
                  }
                  last_info = info;

                  if (target_end >= 0) {
                     long long expected_start = target_end - n;
                     if (expected_start > produced) {
                        long long missing = expected_start - produced;
                        int lost_n = speex_opus_decode_loss(dec,
                              (int)(missing * 1000000 / rate), pcm,
                              SPEEX_OPUS_MAX_FRAME_SAMPLES);
                        if (lost_n < 0)
                           goto done;
                        if (opus_info && !quiet)
                           fprintf(stderr, "opus conceal duration_us=%lld\n",
                                   missing * 1000000 / rate);
                        opus_emit_pcm(fout, outFile, pcm, le_pcm, lost_n,
                                      channels, &audio_size);
                        produced += lost_n;
                        last_info = info;
                     }
                  }
               }

               if (trim > 0) {
                  int skip = trim < n ? trim : n;
                  memmove(pcm, pcm + skip * channels,
                          (size_t)(n - skip) * channels * sizeof(*pcm));
                  n -= skip;
                  trim -= skip;
               }
               if (target_end >= 0 && produced + n > target_end)
                  n = target_end > produced ? (int)(target_end - produced) : 0;

               if (n > 0) {
                  opus_emit_pcm(fout, outFile, pcm, le_pcm, n, channels,
                                &audio_size);
                  produced += n;
               }
               if (opus_info && !quiet)
                  fprintf(stderr, "opus packet frames=%d frame_us=%d "
                          "rate=%d duration_us=%d concealed=%d\n",
                          last_info.frames_per_packet,
                          last_info.frame_duration_us,
                          last_info.sample_rate,
                          last_info.packet_duration_us,
                          last_info.concealed);
               if (op.e_o_s)
                  eos = 1;
            }
         }
      }
      if (nb_read == 0)
         break;
   }

   if (fout && wav_format) {
      if (fseek(fout, 4, SEEK_SET) == 0) {
         int tmp = le_int(audio_size + 36);
         fwrite(&tmp, 4, 1, fout);
         if (fseek(fout, 32, SEEK_CUR) == 0) {
            tmp = le_int(audio_size);
            fwrite(&tmp, 4, 1, fout);
         }
      }
   }
   result = 0;

done:
   if (dec)
      speex_opus_decoder_destroy(dec);
   free(pcm);
   free(le_pcm);
   if (stream_init)
      ogg_stream_clear(&os);
   ogg_sync_clear(&oy);
#if defined WIN32 || defined _WIN32
   if (strlen(outFile) == 0)
      WIN_Audio_close();
#endif
   if (fout)
      fclose(fout);
   return result;
}

void usage()
{
   printf ("Usage: speexdec [options] input_file.spx [output_file]\n");
   printf ("\n");
   printf ("Decodes a Speex file and produce a WAV file or raw file\n");
   printf ("\n");
   printf ("input_file can be:\n");
   printf ("  filename.spx         regular Speex file\n");
   printf ("  -                    stdin\n");
   printf ("\n");
   printf ("output_file can be:\n");
   printf ("  filename.wav         Wav file\n");
   printf ("  filename.*           Raw PCM file (any extension other that .wav)\n");
   printf ("  -                    stdout\n");
   printf ("  (nothing)            Will be played to soundcard\n");
   printf ("\n");
   printf ("Options:\n");
   printf (" --enh                 Enable perceptual enhancement (default)\n");
   printf (" --no-enh              Disable perceptual enhancement\n");
   printf (" --force-nb            Force decoding in narrowband\n");
   printf (" --force-wb            Force decoding in wideband\n");
   printf (" --force-uwb           Force decoding in ultra-wideband\n");
   printf (" --mono                Force decoding in mono\n");
   printf (" --stereo              Force decoding in stereo\n");
   printf (" --rate n              Force decoding at sampling rate n Hz\n");
   printf (" --packet-loss n       Simulate n %% random packet loss\n");
   printf (" --opus-info           Print per-packet Opus timing metadata\n");
   printf (" -V                    Verbose mode (show bit-rate)\n");
   printf (" -h, --help            This help\n");
   printf (" -v, --version         Version information\n");
   printf (" --pf                  Deprecated, use --enh instead\n");
   printf (" --no-pf               Deprecated, use --no-enh instead\n");
   printf ("\n");
   printf ("More information is available from the Speex site: http://www.speex.org\n");
   printf ("\n");
   printf ("Please report bugs to the mailing list `speex-dev@xiph.org'.\n");
}

void version()
{
   const char* speex_version;
   speex_lib_ctl(SPEEX_LIB_GET_VERSION_STRING, (void*)&speex_version);
   printf ("speexdec (Speex decoder) version %s (compiled " __DATE__ ")\n", speex_version);
   printf ("Copyright (C) 2002-2006 Jean-Marc Valin\n");
}

void version_short()
{
   const char* speex_version;
   speex_lib_ctl(SPEEX_LIB_GET_VERSION_STRING, (void*)&speex_version);
   printf ("speexdec version %s\n", speex_version);
   printf ("Copyright (C) 2002-2006 Jean-Marc Valin\n");
}

static void *process_header(ogg_packet *op, spx_int32_t enh_enabled, spx_int32_t *frame_size, int *granule_frame_size, spx_int32_t *rate, int *nframes, int forceMode, int *channels, SpeexStereoState *stereo, int *extra_headers, int quiet)
{
   void *st;
   const SpeexMode *mode;
   SpeexHeader *header;
   int modeID;
   SpeexCallback callback;

   header = speex_packet_to_header((char*)op->packet, op->bytes);
   if (!header)
   {
      fprintf (stderr, "Cannot read header\n");
      return NULL;
   }
   if (header->mode >= SPEEX_NB_MODES || header->mode<0)
   {
      fprintf (stderr, "Mode number %d does not (yet/any longer) exist in this version\n",
               header->mode);
      free(header);
      return NULL;
   }

   modeID = header->mode;
   if (forceMode!=-1)
      modeID = forceMode;

   mode = speex_lib_get_mode (modeID);

   if (header->speex_version_id > 1)
   {
      fprintf (stderr, "This file was encoded with Speex bit-stream version %d, which I don't know how to decode\n", header->speex_version_id);
      free(header);
      return NULL;
   }

   if (mode->bitstream_version < header->mode_bitstream_version)
   {
      fprintf (stderr, "The file was encoded with a newer version of Speex. You need to upgrade in order to play it.\n");
      free(header);
      return NULL;
   }
   if (mode->bitstream_version > header->mode_bitstream_version)
   {
      fprintf (stderr, "The file was encoded with an older version of Speex. You would need to downgrade the version in order to play it.\n");
      free(header);
      return NULL;
   }

   st = speex_decoder_init(mode);
   if (!st)
   {
      fprintf (stderr, "Decoder initialization failed.\n");
      free(header);
      return NULL;
   }
   speex_decoder_ctl(st, SPEEX_SET_ENH, &enh_enabled);
   speex_decoder_ctl(st, SPEEX_GET_FRAME_SIZE, frame_size);
   *granule_frame_size = *frame_size;

   if (!*rate)
      *rate = header->rate;
   /* Adjust rate if --force-* options are used */
   if (forceMode!=-1)
   {
      if (header->mode < forceMode)
      {
         *rate <<= (forceMode - header->mode);
         *granule_frame_size >>= (forceMode - header->mode);
      }
      if (header->mode > forceMode)
      {
         *rate >>= (header->mode - forceMode);
         *granule_frame_size <<= (header->mode - forceMode);
      }
   }


   speex_decoder_ctl(st, SPEEX_SET_SAMPLING_RATE, rate);

   *nframes = header->frames_per_packet;

   if (*channels==-1)
      *channels = header->nb_channels;

   if (!(*channels==1))
   {
      *channels = 2;
      callback.callback_id = SPEEX_INBAND_STEREO;
      callback.func = speex_std_stereo_request_handler;
      callback.data = stereo;
      speex_decoder_ctl(st, SPEEX_SET_HANDLER, &callback);
   }

   if (!quiet)
   {
      fprintf (stderr, "Decoding %d Hz audio using %s mode",
               *rate, mode->modeName);

      if (*channels==1)
         fprintf (stderr, " (mono");
      else
         fprintf (stderr, " (stereo");

      if (header->vbr)
         fprintf (stderr, ", VBR)\n");
      else
         fprintf(stderr, ")\n");
      /*fprintf (stderr, "Decoding %d Hz audio at %d bps using %s mode\n",
       *rate, mode->bitrate, mode->modeName);*/
   }

   *extra_headers = header->extra_headers;

   free(header);
   return st;
}

int main(int argc, char **argv)
{
   int c;
   int option_index = 0;
   char *inFile, *outFile;
   FILE *fin, *fout=NULL;
   short out[MAX_FRAME_SIZE];
   short output[MAX_FRAME_SIZE];
   int frame_size=0, granule_frame_size=0;
   void *st=NULL;
   SpeexBits bits;
   int packet_count=0;
   int stream_init = 0;
   int quiet = 0;
   ogg_int64_t page_granule=0, last_granule=0;
   int skip_samples=0, page_nb_packets;
   struct option long_options[] =
   {
      {"help", no_argument, NULL, 0},
      {"quiet", no_argument, NULL, 0},
      {"version", no_argument, NULL, 0},
      {"version-short", no_argument, NULL, 0},
      {"enh", no_argument, NULL, 0},
      {"no-enh", no_argument, NULL, 0},
      {"pf", no_argument, NULL, 0},
      {"no-pf", no_argument, NULL, 0},
      {"force-nb", no_argument, NULL, 0},
      {"force-wb", no_argument, NULL, 0},
      {"force-uwb", no_argument, NULL, 0},
      {"rate", required_argument, NULL, 0},
      {"mono", no_argument, NULL, 0},
      {"stereo", no_argument, NULL, 0},
      {"packet-loss", required_argument, NULL, 0},
      {"opus-info", no_argument, NULL, 0},
      {0, 0, 0, 0}
   };
   ogg_sync_state oy;
   ogg_page       og;
   ogg_packet     op;
   ogg_stream_state os;
   int enh_enabled;
   int nframes=2;
   int print_bitrate=0;
   int close_in=0;
   int eos=0;
   int forceMode=-1;
   int audio_size=0;
   float loss_percent=-1;
   SpeexStereoState stereo = SPEEX_STEREO_STATE_INIT;
   int channels=-1;
   int rate=0;
   int extra_headers=0;
   int wav_format=0;
   int lookahead;
   int speex_serialno = -1;
   int opus_info = 0;
   unsigned char magic[8];
   int magic_len = 0;

   enh_enabled = 1;

   /*Process options*/
   while(1)
   {
      c = getopt_long (argc, argv, "hvV",
                       long_options, &option_index);
      if (c==-1)
         break;

      switch(c)
      {
      case 0:
         if (strcmp(long_options[option_index].name,"help")==0)
         {
            usage();
            exit(0);
         } else if (strcmp(long_options[option_index].name,"quiet")==0)
         {
            quiet = 1;
         } else if (strcmp(long_options[option_index].name,"version")==0)
         {
            version();
            exit(0);
         } else if (strcmp(long_options[option_index].name,"version-short")==0)
         {
            version_short();
            exit(0);
         } else if (strcmp(long_options[option_index].name,"enh")==0)
         {
            enh_enabled=1;
         } else if (strcmp(long_options[option_index].name,"no-enh")==0)
         {
            enh_enabled=0;
         } else if (strcmp(long_options[option_index].name,"pf")==0)
         {
            fprintf (stderr, "--pf is deprecated, use --enh instead\n");
            enh_enabled=1;
         } else if (strcmp(long_options[option_index].name,"no-pf")==0)
         {
            fprintf (stderr, "--no-pf is deprecated, use --no-enh instead\n");
            enh_enabled=0;
         } else if (strcmp(long_options[option_index].name,"force-nb")==0)
         {
            forceMode=0;
         } else if (strcmp(long_options[option_index].name,"force-wb")==0)
         {
            forceMode=1;
         } else if (strcmp(long_options[option_index].name,"force-uwb")==0)
         {
            forceMode=2;
         } else if (strcmp(long_options[option_index].name,"mono")==0)
         {
            channels=1;
         } else if (strcmp(long_options[option_index].name,"stereo")==0)
         {
            channels=2;
         } else if (strcmp(long_options[option_index].name,"rate")==0)
         {
            rate=atoi (optarg);
         } else if (strcmp(long_options[option_index].name,"packet-loss")==0)
         {
            loss_percent = atof(optarg);
         } else if (strcmp(long_options[option_index].name,"opus-info")==0)
         {
            opus_info = 1;
         }
         break;
      case 'h':
         usage();
         exit(0);
         break;
      case 'v':
         version();
         exit(0);
         break;
      case 'V':
         print_bitrate=1;
         break;
      case '?':
         usage();
         exit(1);
         break;
      }
   }
   if (argc-optind!=2 && argc-optind!=1)
   {
      usage();
      exit(1);
   }
   inFile=argv[optind];

   if (argc-optind==2)
      outFile=argv[optind+1];
   else
      outFile = "";
   wav_format = strlen(outFile)>=4 && (
                                       strcmp(outFile+strlen(outFile)-4,".wav")==0
                                       || strcmp(outFile+strlen(outFile)-4,".WAV")==0);
   /*Open input file*/
   if (strcmp(inFile, "-")==0)
   {
#if defined WIN32 || defined _WIN32
      _setmode(_fileno(stdin), _O_BINARY);
#endif
      fin=stdin;
   }
   else
   {
      fin = fopen(inFile, "rb");
      if (!fin)
      {
         perror(inFile);
         exit(1);
      }
      close_in=1;
   }

   magic_len = (int)fread(magic, 1, sizeof(magic), fin);
   if (magic_len == (int)sizeof(magic) &&
       (!memcmp(magic, "OpusHead", 8) || !memcmp(magic, "OggS", 4)))
   {
      int rc;
      if (fseek(fin, 0, SEEK_SET) != 0)
      {
         /* Non-seekable Speex input: feed the prefix to the Ogg parser. */
         if (memcmp(magic, "OpusHead", 8) == 0)
         {
            fprintf(stderr, "Cannot rewind Opus input\n");
            if (close_in)
               fclose(fin);
            return 1;
         }
      }
      else
      {
         rc = decode_opus_ogg(fin, outFile, wav_format, loss_percent, quiet,
                              opus_info, rate, channels);
         if (rc != 2)
         {
            if (close_in)
               fclose(fin);
            return rc;
         }
         if (fseek(fin, 0, SEEK_SET) != 0)
         {
            if (close_in)
               fclose(fin);
            return 1;
         }
         magic_len = 0;
      }
   }

   /*Init Ogg data struct*/
   ogg_sync_init(&oy);

   speex_bits_init(&bits);
   /*Main decoding loop*/

   while (1)
   {
      char *data;
      int i, j, nb_read;
      if (magic_len > 0) {
         data = ogg_sync_buffer(&oy, magic_len);
         memcpy(data, magic, magic_len);
         ogg_sync_wrote(&oy, magic_len);
         nb_read = magic_len;
         magic_len = 0;
      } else {
         /*Get the ogg buffer for writing*/
         data = ogg_sync_buffer(&oy, 200);
         /*Read bitstream from input file*/
         nb_read = fread(data, sizeof(char), 200, fin);
         if (nb_read < 200 ) {
            int err = ferror(fin);
            if (err) {
               fprintf(stderr, "Read error: %s\n", strerror(err));
               break;
            }
         }
         ogg_sync_wrote(&oy, nb_read);
      }

      /*Loop for all complete pages we got (most likely only one)*/
      while (ogg_sync_pageout(&oy, &og)==1)
      {
         int packet_no;
         if (stream_init == 0) {
            ogg_stream_init(&os, ogg_page_serialno(&og));
            stream_init = 1;
         }
         if (ogg_page_serialno(&og) != os.serialno) {
            /* so all streams are read. */
            ogg_stream_reset_serialno(&os, ogg_page_serialno(&og));
         }
         /*Add page to the bitstream*/
         ogg_stream_pagein(&os, &og);
         page_granule = ogg_page_granulepos(&og);
         page_nb_packets = ogg_page_packets(&og);
         if (page_granule>0 && frame_size)
         {
            /* FIXME: shift the granule values if --force-* is specified */
            skip_samples = frame_size*(page_nb_packets*granule_frame_size*nframes - (page_granule-last_granule))/granule_frame_size;
            if (ogg_page_eos(&og))
               skip_samples = -skip_samples;
            /*else if (!ogg_page_bos(&og))
               skip_samples = 0;*/
         } else
         {
            skip_samples = 0;
         }
         /*printf ("page granulepos: %d %d %d\n", skip_samples, page_nb_packets, (int)page_granule);*/
         last_granule = page_granule;
         /*Extract all available packets*/
         packet_no=0;
         while (!eos && ogg_stream_packetout(&os, &op) == 1)
         {
            if (op.bytes>=5 && !memcmp(op.packet, "Speex", 5)) {
               speex_serialno = os.serialno;
            }
            if (speex_serialno == -1 || os.serialno != speex_serialno)
               break;
            /*If first packet, process as Speex header*/
            if (packet_count==0)
            {
               st = process_header(&op, enh_enabled, &frame_size, &granule_frame_size, &rate, &nframes, forceMode, &channels, &stereo, &extra_headers, quiet);
               if (!st)
                  exit(1);
               speex_decoder_ctl(st, SPEEX_GET_LOOKAHEAD, &lookahead);
               if (!nframes)
                  nframes=1;
               fout = out_file_open(outFile, rate, &channels);

            } else if (packet_count==1)
            {
               if (!quiet)
                  print_comments((char*)op.packet, op.bytes);
            } else if (packet_count<=1+extra_headers)
            {
               /* Ignore extra headers */
            } else {
               int lost=0;
               packet_no++;
               if (loss_percent>0 && 100*((float)rand())/RAND_MAX<loss_percent)
                  lost=1;

               /*End of stream condition*/
               if (op.e_o_s && os.serialno == speex_serialno) /* don't care for anything except speex eos */
                  eos=1;

               /*Copy Ogg packet to Speex bitstream*/
               speex_bits_read_from(&bits, (char*)op.packet, op.bytes);
               for (j=0;j!=nframes;j++)
               {
                  int ret;
                  /*Decode frame*/
                  if (!lost)
                     ret = speex_decode_int(st, &bits, output);
                  else
                     ret = speex_decode_int(st, NULL, output);

                  /*for (i=0;i<frame_size*channels;i++)
                    printf ("%d\n", (int)output[i]);*/

                  if (ret==-1)
                     break;
                  if (ret==-2)
                  {
                     fprintf (stderr, "Decoding error: corrupted stream?\n");
                     break;
                  }
                  if (speex_bits_remaining(&bits)<0)
                  {
                     fprintf (stderr, "Decoding overflow: corrupted stream?\n");
                     break;
                  }
                  if (channels==2)
                     speex_decode_stereo_int(output, frame_size, &stereo);

                  if (print_bitrate) {
                     spx_int32_t tmp;
                     char ch=13;
                     speex_decoder_ctl(st, SPEEX_GET_BITRATE, &tmp);
                     fputc (ch, stderr);
                     fprintf (stderr, "Bitrate is use: %d bps     ", tmp);
                  }
                  /*Convert to short and save to output file*/
                  if (strlen(outFile)!=0)
                  {
                     for (i=0;i<frame_size*channels;i++)
                        out[i]=le_short(output[i]);
                  } else {
                     for (i=0;i<frame_size*channels;i++)
                        out[i]=output[i];
                  }
                  {
                     int frame_offset = 0;
                     int new_frame_size = frame_size;
                     /*printf ("packet %d %d\n", packet_no, skip_samples);*/
                     /*fprintf (stderr, "packet %d %d %d\n", packet_no, skip_samples, lookahead);*/
                     if (packet_no == 1 && j==0 && skip_samples > 0)
                     {
                        /*printf ("chopping first packet\n");*/
                        new_frame_size -= skip_samples+lookahead;
                        frame_offset = skip_samples+lookahead;
                     }
                     if (packet_no == page_nb_packets && skip_samples < 0)
                     {
                        int packet_length = nframes*frame_size+skip_samples+lookahead;
                        new_frame_size = packet_length - j*frame_size;
                        if (new_frame_size<0)
                           new_frame_size = 0;
                        if (new_frame_size>frame_size)
                           new_frame_size = frame_size;
                        /*printf ("chopping end: %d %d %d\n", new_frame_size, packet_length, packet_no);*/
                     }
                     if (new_frame_size>0)
                     {
#if defined WIN32 || defined _WIN32
                        if (strlen(outFile)==0)
                           WIN_Play_Samples (out+frame_offset*channels, sizeof(short) * new_frame_size*channels);
                        else
#endif
                           fwrite(out+frame_offset*channels, sizeof(short), new_frame_size*channels, fout);

                        audio_size+=sizeof(short)*new_frame_size*channels;
                     }
                  }
               }
            }
            packet_count++;
         }
      }
      if (feof(fin))
         break;

      if (nb_read == 0)
         break;
   }

   if (fout && wav_format)
   {
      if (fseek(fout,4,SEEK_SET)==0)
      {
         int tmp;
         tmp = le_int(audio_size+36);
         fwrite(&tmp,4,1,fout);
         if (fseek(fout,32,SEEK_CUR)==0)
         {
            tmp = le_int(audio_size);
            fwrite(&tmp,4,1,fout);
         } else
         {
            fprintf (stderr, "First seek worked, second didn't\n");
         }
      } else {
         fprintf (stderr, "Cannot seek on wave file, size will be incorrect\n");
      }
   }

   if (st)
      speex_decoder_destroy(st);
   else
   {
      fprintf (stderr, "This doesn't look like a Speex file\n");
   }
   speex_bits_destroy(&bits);
   if (stream_init)
      ogg_stream_clear(&os);
   ogg_sync_clear(&oy);

#if defined WIN32 || defined _WIN32
   if (strlen(outFile)==0)
      WIN_Audio_close ();
#endif

   if (close_in)
      fclose(fin);
   if (fout != NULL)
      fclose(fout);

   return 0;
}
