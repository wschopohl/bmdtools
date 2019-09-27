/*
 * Blackmagic Devices Decklink capture
 * Copyright (c) 2013 Luca Barbato.
 *
 * This file is part of bmdtools.
 *
 * bmdtools is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * bmdtools is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with bmdtools; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <libgen.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include "libswscale/swscale.h"
}
#include "compat.h"
#include "Play.h"

#include "modes.h"

pthread_mutex_t sleepMutex;
pthread_cond_t sleepCond;
IDeckLinkConfiguration *deckLinkConfiguration;

AVFormatContext *ic;
AVFrame *avframe;

typedef struct PlayStream {
    AVStream *st;
    AVCodecContext *codec;
} PlayStream;

PlayStream audio;
PlayStream video;

static enum AVPixelFormat pix_fmt = AV_PIX_FMT_UYVY422;
static BMDPixelFormat pix         = bmdFormat8BitYUV;

static int buffer    = 2000 * 1000;
static int serial_fd = -1;

const unsigned long kAudioWaterlevel = 48000 / 4;      /* small */

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    uint64_t nb_packets;
    int size;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

PacketQueue audioqueue;
PacketQueue videoqueue;
PacketQueue dataqueue;
struct SwsContext *sws;

static void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt   = NULL;
    q->first_pkt  = NULL;
    q->nb_packets = 0;
    q->size       = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_end(PacketQueue *q)
{
    packet_queue_flush(q);
    pthread_mutex_lock(&q->mutex);
    q->abort_request = -1;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt  = *pkt;
    pkt1->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (!q->last_pkt)

        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    if (q->nb_packets > 5000)
        fprintf(stderr,
                "%" PRId64 " storing %p, %s - is the input faster than realtime?\n",
                q->nb_packets,
                q,
                q == &videoqueue ? "videoqueue" : "audioqueue");
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for (;; ) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            if (q->nb_packets > 5000)
                fprintf(stderr, "pulling %" PRId64 " from %p %s\n",
                        q->nb_packets,
                        q,
                        q == &videoqueue ? "videoqueue" : "audioqueue");
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt     = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            if (q->abort_request) {
                ret = -1;
                break;
            }
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

int64_t first_audio_pts = AV_NOPTS_VALUE;
int64_t first_video_pts = AV_NOPTS_VALUE;
int64_t first_pts       = AV_NOPTS_VALUE;
int fill_me             = 1;

void *fill_queues(void *unused)
{
    AVPacket pkt;
    AVStream *st;
    int once = 0;

    while (fill_me) {
        int err = av_read_frame(ic, &pkt);
        if (err) {
            pthread_cond_signal(&sleepCond);
            return NULL;
        }
        if (videoqueue.nb_packets > 1000) {
            if (!once++)
                fprintf(stderr, "Queue size %d problems ahead\n",
                        videoqueue.size);
        }
        st = ic->streams[pkt.stream_index];
        switch (st->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (pkt.pts != AV_NOPTS_VALUE) {
                if (first_pts == AV_NOPTS_VALUE) {
                    first_pts       = first_video_pts = pkt.pts;
                    if (audio.st) {
                        first_audio_pts =
                            av_rescale_q(pkt.pts, video.st->time_base,
                                         audio.st->time_base);
                    }
                }
                pkt.pts -= first_video_pts;
            }
            packet_queue_put(&videoqueue, &pkt);
            break;
        case AVMEDIA_TYPE_AUDIO:
            if (pkt.pts != AV_NOPTS_VALUE) {
                if (first_pts == AV_NOPTS_VALUE) {
                    first_pts       = first_audio_pts = pkt.pts;
                    first_video_pts =
                        av_rescale_q(pkt.pts, audio.st->time_base,
                                     video.st->time_base);
                }
                pkt.pts -= first_audio_pts;
            }
            packet_queue_put(&audioqueue, &pkt);
            break;
        case AVMEDIA_TYPE_DATA:
	    packet_queue_put(&dataqueue, &pkt);
            break;
        default:
            av_packet_unref(&pkt);
            break;
        }
    }
    return NULL;
}

void sigfunc(int signum)
{
    pthread_cond_signal(&sleepCond);
}

int usage(int status)
{
    HRESULT result;
    IDeckLinkIterator *deckLinkIterator;
    IDeckLink *deckLink;
    int numDevices = 0;

    fprintf(stderr,
            "Usage: bmdplay -m <mode id> [OPTIONS]\n"
            "\n"
            "    -m <mode id>:\n"
            );

    // Create an IDeckLinkIterator object to enumerate all DeckLink cards in the system
    deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (deckLinkIterator == NULL) {
        fprintf(
            stderr,
            "A DeckLink iterator could not be created.  The DeckLink drivers may not be installed.\n");
        return 1;
    }

    // Enumerate all cards in this system
    while (deckLinkIterator->Next(&deckLink) == S_OK) {
        BMDProbeString str;

        // Increment the total number of DeckLink cards found
        numDevices++;
        if (numDevices > 1)
            printf("\n\n");

        // *** Print the model name of the DeckLink card
        result = deckLink->GetModelName(&str);
        if (result == S_OK) {
            printf("-> %s (-C %d )\n\n",
                   ToStr(str),
                   numDevices - 1);
            FreeStr(str);
        }

        print_output_modes(deckLink);
        // Release the IDeckLink instance when we've finished with it to prevent leaks
        deckLink->Release();
    }
    deckLinkIterator->Release();

    // If no DeckLink cards were found in the system, inform the user
    if (numDevices == 0)
        printf("No Blackmagic Design devices were found.\n");
    printf("\n");

    fprintf(
        stderr,
        "    -f <filename>        Filename of input video file\n"
        "    -C <num>             Card number to be used\n"
        "    -b <num>             Milliseconds of pre-buffering before playback (default = 2000 ms)\n"
        "    -p <pixel>           PixelFormat Depth (8 or 10 - default is 8)\n"
        "    -S <port>            Serial device (i.e: /dev/ttyS0, /dev/ttyUSB0)\n"
        "    -O <output>          Output connection:\n"
        "                         1: Composite video + analog audio\n"
        "                         2: Components video + analog audio\n"
        "                         3: HDMI video + audio\n"
        "                         4: SDI video + audio\n\n");

    return status;
}

int main(int argc, char *argv[])
{
    Player generator;
    int ch, ret;
    int videomode  = 2;
    int connection = 0;
    int camera     = 0;
    char *filename = NULL;

    while ((ch = getopt(argc, argv, "?hs:f:a:m:n:F:C:O:b:p:S:")) != -1) {
        switch (ch) {
        case 'p':
            switch (atoi(optarg)) {
            case  8:
                pix     = bmdFormat8BitYUV;
                pix_fmt = AV_PIX_FMT_UYVY422;
                break;
            case 10:
                pix     = bmdFormat10BitYUV;
                pix_fmt = AV_PIX_FMT_YUV422P10;
                break;
            default:
                fprintf(
                    stderr,
                    "Invalid argument: Pixel Format Depth must be either 8 bits or 10 bits\n");
                return usage(1);
            }
            break;
        case 'f':
            filename = strdup(optarg);
            break;
        case 'm':
            videomode = atoi(optarg);
            break;
        case 'O':
            connection = atoi(optarg);
            break;
        case 'C':
            camera = atoi(optarg);
            break;
        case 'b':
            buffer = atoi(optarg) * 1000;
            break;
        case 'S':
            serial_fd = open(optarg, O_RDWR | O_NONBLOCK);
            break;
        case '?':
        case 'h':
            return usage(0);
        }
    }

    if (!filename)
        return usage(1);

    av_register_all();
    ic = avformat_alloc_context();

    avformat_open_input(&ic, filename, NULL, NULL);
    avformat_find_stream_info(ic, NULL);

    for (int i = 0; i < ic->nb_streams; i++) {
        AVStream *st           = ic->streams[i];
        AVCodecParameters *par = st->codecpar;
        AVCodec *codec         = avcodec_find_decoder(par->codec_id);
        switch (par->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
        case AVMEDIA_TYPE_VIDEO:
            if (codec) {
                AVCodecContext *avctx  = avcodec_alloc_context3(codec);
                if (!avctx) {
                    av_log(NULL, AV_LOG_ERROR, "Out of memory\n");
                    exit(1);
                }

                if (avcodec_parameters_to_context(avctx, par) < 0 ||
                    avcodec_open2(avctx, codec, NULL) < 0) {
                    avcodec_free_context(&avctx);
                    av_log(NULL, AV_LOG_ERROR, "Codec open failed\n");
                    exit(1);
                }

                if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
                    audio.st    = st;
                    audio.codec = avctx;
                }

                if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                    video.st    = st;
                    video.codec = avctx;
                }
            } else {
                fprintf(
                    stderr, "cannot find codecs for %s\n",
                    (par->codec_type ==
                     AVMEDIA_TYPE_AUDIO) ? "Audio" : "Video");
                continue;
            }
            break;
        default:
            av_log(NULL, AV_LOG_VERBOSE, "Skipping stream %d\n", i);
        }
    }

    if (!audio.st) {
        av_log(NULL, AV_LOG_INFO,
               "No audio stream found - bmdplay will just play video\n");
    }

    if (!video.st) {
        av_log(NULL, AV_LOG_ERROR,
               "No video stream found - bmdplay will close now.\n");
        return 1;
    }

    av_dump_format(ic, 0, filename, 0);

    sws = sws_getContext(video.st->codecpar->width,
                         video.st->codecpar->height,
                         (AVPixelFormat)video.st->codecpar->format,
                         video.st->codecpar->width,
                         video.st->codecpar->height,
                         pix_fmt,
                         SWS_BILINEAR, NULL, NULL, NULL);

    signal(SIGINT, sigfunc);
    pthread_mutex_init(&sleepMutex, NULL);
    pthread_cond_init(&sleepCond, NULL);

    free(filename);

    ret = generator.Init(videomode, connection, camera);

    avformat_close_input(&ic);

    fprintf(stderr, "video %" PRId64 " audio %" PRId64 "\n",
            videoqueue.nb_packets,
            audioqueue.nb_packets);

    return ret;
}

Player::Player()
{
    m_audioSampleRate = bmdAudioSampleRate48kHz;
    m_running         = false;
    m_outputSignal    = kOutputSignalDrop;
}

bool Player::Init(int videomode, int connection, int camera)
{
    // Initialize the DeckLink API
    IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
    HRESULT result;
    int i = 0;

    if (!deckLinkIterator) {
        fprintf(stderr,
                "This application requires the DeckLink drivers installed.\n");
        goto bail;
    }

    if (audio.st) {
        m_audioSampleDepth =
            av_get_exact_bits_per_sample(audio.codec->codec_id);

        switch (audio.codec->channels) {
            case  2:
            case  8:
            case 16:
                break;
            default:
                fprintf(stderr,
                        "%d channels not supported, please use 2, 8 or 16\n",
                        audio.codec->channels);
                goto bail;
        }

        switch (m_audioSampleDepth) {
            case 16:
            case 32:
                break;
            default:
                fprintf(stderr, "%lubit audio not supported use 16bit or 32bit\n",
                        m_audioSampleDepth);
        }
    }

    do
        result = deckLinkIterator->Next(&m_deckLink);
    while (i++ < camera);

    if (result != S_OK) {
        fprintf(stderr, "No DeckLink PCI cards found\n");
        goto bail;
    }

    // Obtain the audio/video output interface (IDeckLinkOutput)
    if (m_deckLink->QueryInterface(IID_IDeckLinkOutput,
                                   (void **)&m_deckLinkOutput) != S_OK)
        goto bail;

    result = m_deckLink->QueryInterface(IID_IDeckLinkConfiguration,
                                        (void **)&deckLinkConfiguration);
    if (result != S_OK) {
        fprintf(
            stderr,
            "Could not obtain the IDeckLinkConfiguration interface - result = %08x\n",
            result);
        goto bail;
    }
    //XXX make it generic
    switch (connection) {
    case 1:
        DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionComposite);
        DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionAnalog);
        break;
    case 2:
        DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionComponent);
        DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionAnalog);
        break;
    case 3:
        DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionHDMI);
        DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionEmbedded);
        break;
    case 4:
        DECKLINK_SET_VIDEO_CONNECTION(bmdVideoConnectionSDI);
        DECKLINK_SET_AUDIO_CONNECTION(bmdAudioConnectionEmbedded);
        break;
    default:
        // do not change it
        break;
    }

    // Provide this class as a delegate to the audio and video output interfaces
    m_deckLinkOutput->SetScheduledFrameCompletionCallback(this);
    m_deckLinkOutput->SetAudioCallback(this);

    avframe = av_frame_alloc();

    packet_queue_init(&audioqueue);
    packet_queue_init(&videoqueue);
    packet_queue_init(&dataqueue);
    pthread_t th;
    pthread_create(&th, NULL, fill_queues, NULL);

    usleep(buffer); // You can add the microseconds you need for pre-buffering before start playing
    // Start playing
    StartRunning(videomode);

    pthread_mutex_lock(&sleepMutex);
    pthread_cond_wait(&sleepCond, &sleepMutex);
    pthread_mutex_unlock(&sleepMutex);
    fill_me = 0;
    fprintf(stderr, "Exiting, cleaning up\n");
    packet_queue_end(&audioqueue);
    packet_queue_end(&videoqueue);

bail:
    if (m_running == true) {
        StopRunning();
    } else {
        // Release any resources that were partially allocated
        if (m_deckLinkOutput != NULL) {
            m_deckLinkOutput->Release();
            m_deckLinkOutput = NULL;
        }
        //
        if (m_deckLink != NULL) {
            m_deckLink->Release();
            m_deckLink = NULL;
        }
    }

    if (deckLinkIterator != NULL)
        deckLinkIterator->Release();

    return true;
}

IDeckLinkDisplayMode *Player::GetDisplayModeByIndex(int selectedIndex)
{
    // Populate the display mode combo with a list of display modes supported by the installed DeckLink card
    IDeckLinkDisplayModeIterator *displayModeIterator;
    IDeckLinkDisplayMode *deckLinkDisplayMode;
    IDeckLinkDisplayMode *selectedMode = NULL;
    int index                          = 0;

    if (m_deckLinkOutput->GetDisplayModeIterator(&displayModeIterator) != S_OK)
        goto bail;
    while (displayModeIterator->Next(&deckLinkDisplayMode) == S_OK) {
        BMDProbeString str;

        if (deckLinkDisplayMode->GetName(&str) == S_OK) {
            if (index == selectedIndex) {
                printf("Selected mode: %s\n\n\n", ToStr(str));
                selectedMode = deckLinkDisplayMode;
                FreeStr(str);
                goto bail;
            }
        }
        index++;
    }
bail:
    displayModeIterator->Release();
    return selectedMode;
}

void Player::StartRunning(int videomode)
{
    IDeckLinkDisplayMode *videoDisplayMode = NULL;
    unsigned long audioSamplesPerFrame;

    // Get the display mode for 1080i 59.95
    videoDisplayMode = GetDisplayModeByIndex(videomode);

    if (!videoDisplayMode)
        return;

    m_frameWidth  = videoDisplayMode->GetWidth();
    m_frameHeight = videoDisplayMode->GetHeight();
    videoDisplayMode->GetFrameRate(&m_frameDuration, &m_frameTimescale);

    // Set the video output mode
    if (m_deckLinkOutput->EnableVideoOutput(videoDisplayMode->GetDisplayMode(),
                                            bmdVideoOutputFlagDefault) !=
        S_OK) {
        fprintf(stderr, "Failed to enable video output\n");
        return;
    }

    // Set the audio output mode
    if (audio.st) {
        if (m_deckLinkOutput->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                                m_audioSampleDepth,
                                                audio.codec->channels,
                                                bmdAudioOutputStreamTimestamped) !=
            S_OK) {
            fprintf(stderr, "Failed to enable audio output\n");
            return;
        }

        for (unsigned i = 0; i < 10; i++)
            ScheduleNextFrame(true);

        // Begin audio preroll.  This will begin calling our audio callback, which will start the DeckLink output stream.
    //    m_audioBufferOffset = 0;
        if (m_deckLinkOutput->BeginAudioPreroll() != S_OK) {
            fprintf(stderr, "Failed to begin audio preroll\n");
            return;
        }
    } else {
        for (unsigned i = 0; i < 10; i++)
            ScheduleNextFrame(true);

        m_deckLinkOutput->StartScheduledPlayback(0, 100, 1.0);
    }

    m_running = true;

    return;
}

void Player::StopRunning()
{
    // Stop the audio and video output streams immediately
    m_deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
    //
    m_deckLinkOutput->DisableAudioOutput();
    m_deckLinkOutput->DisableVideoOutput();
}

void Player::ScheduleNextFrame(bool prerolling)
{
    AVPacket pkt;
    void *frame;
    int ret;

    if (serial_fd > 0 && packet_queue_get(&dataqueue, &pkt, 0)) {
        if (pkt.data[0] != ' '){
            fprintf(stderr,"written %.*s  \n", pkt.size, pkt.data);
            write(serial_fd, pkt.data, pkt.size);
        }
        av_packet_unref(&pkt);
    }

    if (packet_queue_get(&videoqueue, &pkt, 0) < 0)
        return;

    IDeckLinkMutableVideoFrame *videoFrame;
    m_deckLinkOutput->CreateVideoFrame(m_frameWidth,
                                       m_frameHeight,
                                       m_frameWidth * 2,
                                       pix,
                                       bmdFrameFlagDefault,
                                       &videoFrame);
    videoFrame->GetBytes(&frame);

    avcodec_send_packet(video.codec, &pkt);

    // TODO: support receiving multiple frames
    ret = avcodec_receive_frame(video.codec, avframe);
    if (ret >= 0) {
        uint8_t *data[4];
        int linesize[4];

        av_image_fill_arrays(data, linesize, (uint8_t *)frame,
                             pix_fmt, m_frameWidth, m_frameHeight, 1);

        sws_scale(sws, avframe->data, avframe->linesize, 0, avframe->height,
                  data, linesize);

        if (m_deckLinkOutput->ScheduleVideoFrame(videoFrame,
                                                 pkt.pts *
                                                 video.st->time_base.num,
                                                 pkt.duration *
                                                 video.st->time_base.num,
                                                 video.st->time_base.den) !=
            S_OK)
            fprintf(stderr, "Error scheduling frame\n");
    }
    videoFrame->Release();
    av_packet_unref(&pkt);
}

void Player::WriteNextAudioSamples()
{
    uint32_t samplesWritten = 0;
    AVPacket pkt            = { 0 };
    unsigned int bufferedSamples;
    int got_frame = 0;
    int i;
    int bytes_per_sample =
        av_get_bytes_per_sample(audio.codec->sample_fmt) *
        audio.codec->channels;
    int samples, off = 0;

    m_deckLinkOutput->GetBufferedAudioSampleFrameCount(&bufferedSamples);

    if (bufferedSamples > kAudioWaterlevel)
        return;

    if (!packet_queue_get(&audioqueue, &pkt, 0))
        return;

    samples = pkt.size / bytes_per_sample;

    do {
        if (m_deckLinkOutput->ScheduleAudioSamples(pkt.data +
                                                   off * bytes_per_sample,
                                                   samples,
                                                   pkt.pts + off,
                                                   audio.st->time_base.den / audio.st->time_base.num,
                                                   &samplesWritten) != S_OK)
            fprintf(stderr, "error writing audio sample\n");
        samples -= samplesWritten;
        off     += samplesWritten;
    } while (samples > 0);

    av_packet_unref(&pkt);
}

/************************* DeckLink API Delegate Methods *****************************/

HRESULT Player::ScheduledFrameCompleted(IDeckLinkVideoFrame *completedFrame,
                                        BMDOutputFrameCompletionResult result)
{
    if (fill_me)
        ScheduleNextFrame(false);
    return S_OK;
}

HRESULT Player::ScheduledPlaybackHasStopped()
{
    return S_OK;
}

HRESULT Player::RenderAudioSamples(bool preroll)
{
    if (audio.st) {
        // Provide further audio samples to the DeckLink API until our preferred buffer waterlevel is reached
        WriteNextAudioSamples();

        if (preroll) {
            // Start audio and video output
            m_deckLinkOutput->StartScheduledPlayback(0, 100, 1.0);
        }
    }

    return S_OK;
}
