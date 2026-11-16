#ifndef SPEEX_OPUS_H
#define SPEEX_OPUS_H
/** @defgroup SpeexOpus Opus receive-side adapter
 *  @{
 */

#include "speex_types.h"
#include "speex.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPEEX_MODEID_OPUS
#define SPEEX_MODEID_OPUS 3
#endif

#define SPEEX_OPUS_OK                      0
#define SPEEX_OPUS_ERR_BAD_ARG            -1
#define SPEEX_OPUS_ERR_BAD_PACKET         -2
#define SPEEX_OPUS_ERR_BUFFER_TOO_SMALL   -3
#define SPEEX_OPUS_ERR_UNSUPPORTED        -4
#define SPEEX_OPUS_ERR_INTERNAL           -5

#define SPEEX_OPUS_BANDWIDTH_NARROWBAND       0
#define SPEEX_OPUS_BANDWIDTH_MEDIUMBAND       1
#define SPEEX_OPUS_BANDWIDTH_WIDEBAND         2
#define SPEEX_OPUS_BANDWIDTH_SUPERWIDEBAND    3
#define SPEEX_OPUS_BANDWIDTH_FULLBAND         4

/** Maximum number of Opus frames in one packet. */
#define SPEEX_OPUS_MAX_FRAMES 48
/** Maximum packet duration, in microseconds. */
#define SPEEX_OPUS_MAX_PACKET_DURATION_US 120000
/** Largest output frame, in samples per channel, at 48 kHz. */
#define SPEEX_OPUS_MAX_FRAME_SAMPLES 5760

/** Control requests understood by speex_opus_decoder_ctl(). */
#define SPEEX_OPUS_GET_PACKET_INFO       2000
#define SPEEX_OPUS_GET_OUTPUT_RATE       2001
#define SPEEX_OPUS_GET_DECLARED_RATE     2002
#define SPEEX_OPUS_GET_LAST_FRAME_SIZE   2003
#define SPEEX_OPUS_GET_LOSS_STREAK       2004
#define SPEEX_OPUS_GET_LOOKAHEAD         2005
#define SPEEX_OPUS_GET_ERROR             2006

/** Description of the timing and channel layout declared by an Opus TOC. */
typedef struct SpeexOpusPacketInfo {
   int bandwidth;            /**< SPEEX_OPUS_BANDWIDTH_* value */
   int sample_rate;          /**< Rate declared by the TOC bandwidth, in Hz */
   int channels;             /**< 1 or 2, from the TOC stereo bit */
   int frame_duration_us;    /**< Duration of one Opus frame, in microseconds */
   int frames_per_packet;    /**< Number of frames carried by this packet */
   int packet_duration_us;   /**< Total packet duration, in microseconds */
   int frame_size;           /**< Per-frame samples at sample_rate */
   int packet_samples;       /**< Total samples at sample_rate */
   int toc;                  /**< Raw TOC byte */
   int payload_offset;       /**< Offset of the first frame payload */
   int packet_bytes;         /**< Encoded packet size */
   int concealed;            /**< Non-zero if this output was PLC/comfort noise */
} SpeexOpusPacketInfo;

typedef struct SpeexOpusDecoder SpeexOpusDecoder;

/** Query packet timing without decoding it.
 *
 * @return SPEEX_OPUS_OK on success, SPEEX_OPUS_ERR_BAD_PACKET if malformed.
 */
int speex_opus_packet_parse(const void *packet, int len,
                            SpeexOpusPacketInfo *info);

/** Create a receive-side decoder.
 *
 * @param output_rate Output rate in Hz: 8000, 12000, 16000, 24000 or 48000.
 *                    0 selects 48000.
 * @param channels    Output channel count (1 or 2).
 * @return Decoder state, or NULL on invalid arguments or allocation failure.
 */
SpeexOpusDecoder *speex_opus_decoder_init(int output_rate, int channels);

/** Alias for speex_opus_decoder_init(). */
SpeexOpusDecoder *speex_opus_decoder_create(int output_rate, int channels);

/** Change the output rate/channel count. This resets decoder history. */
int speex_opus_decoder_configure(SpeexOpusDecoder *st, int output_rate,
                                 int channels);

/** Destroy a decoder created by speex_opus_decoder_init(). */
void speex_opus_decoder_destroy(SpeexOpusDecoder *st);

/** Alias for speex_opus_decoder_destroy(). */
void speex_opus_decoder_free(SpeexOpusDecoder *st);

/** Decode one Opus packet.
 *
 * A packet may contain several Opus frames. The return value is the number of
 * samples per channel written to pcm, matching the packet duration at the
 * configured output rate. A corrupt packet is concealed with PLC instead of
 * terminating the stream; info->concealed is then set.
 *
 * @param packet      Encoded packet, or NULL for a lost packet.
 * @param len         Packet length in bytes (0 for DTX/loss).
 * @param pcm         Interleaved 16-bit output.
 * @param max_samples Samples per channel that fit in pcm.
 * @param info        Optional packet metadata, updated on return.
 * @return Samples per channel, or a negative SPEEX_OPUS_ERR_* value.
 */
int speex_opus_decode(SpeexOpusDecoder *st, const void *packet, int len,
                      spx_int16_t *pcm, int max_samples,
                      SpeexOpusPacketInfo *info);

/** Alias for speex_opus_decode(). */
int speex_opus_decode_packet(SpeexOpusDecoder *st, const void *packet, int len,
                             spx_int16_t *pcm, int max_samples,
                             SpeexOpusPacketInfo *info);

/** Generate concealment audio for a lost packet.
 *
 * The duration should be the duration of the missing packet. Consecutive calls
 * are supported and keep the decoder history intact for the next good packet.
 * If lost_duration_us is 0, the duration of the previous packet is used.
 */
int speex_opus_decode_loss(SpeexOpusDecoder *st, int lost_duration_us,
                           spx_int16_t *pcm, int max_samples);

/** Retrieve the metadata of the most recently decoded or concealed packet. */
int speex_opus_decoder_get_info(SpeexOpusDecoder *st,
                                SpeexOpusPacketInfo *info);

/** Control interface; also used by the SpeexMode adapter. */
int speex_opus_decoder_ctl(SpeexOpusDecoder *st, int request, void *ptr);

/** Algorithmic receive latency in microseconds: decoder lookahead plus the
 *  duration of the last packet. It excludes network packetization and jitter
 *  buffer delay. Returns a negative error value for invalid state.
 */
int speex_opus_decoder_get_latency_us(SpeexOpusDecoder *st);

/** Opus decoder expressed as a SpeexMode so existing receive loops can select
 *  it without changing the Speex bit-stream API. The encoder side is not
 *  implemented by this mode. The bridge uses mono 48 kHz output; use the
 *  speex_opus_* API when stereo or a non-default output rate is required.
 */
extern const SpeexMode speex_opus_mode;

#ifdef __cplusplus
}
#endif

/** @} */
#endif
