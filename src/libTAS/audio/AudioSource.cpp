/*
    Copyright 2015-2016 Clément Gallet <clement.gallet@ens-lyon.org>

    This file is part of libTAS.

    libTAS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libTAS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libTAS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "AudioSource.h"
#include <iterator>     // std::back_inserter
#include <algorithm>    // std::copy
#include "../logging.h"
#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
extern "C" {
    #include <libavutil/opt.h>
    #include <libavutil/channel_layout.h>
}
#endif
#include <stdlib.h>
#include "../DeterministicTimer.h" // detTimer.fakeAdvanceTimer()

/* Helper function to convert ticks into a number of bytes in the audio buffer */
int AudioSource::ticksToSamples(struct timespec ticks, int frequency)
{
    uint64_t nsecs = static_cast<uint64_t>(ticks.tv_sec) * 1000000000 + ticks.tv_nsec;
    uint64_t samples = (nsecs * frequency) / 1000000000;
    samples_frac += (nsecs * frequency) % 1000000000;
    if (samples_frac >= 500000000) {
        samples_frac -= 1000000000;
        samples++;
    }
    return static_cast<int>(samples);
}

AudioSource::AudioSource(void)
{
    id = 0;
    position = 0;
    samples_frac = 0;
    volume = 1.0f;
    source = SOURCE_UNDETERMINED;
    looping = false;
    state = SOURCE_INITIAL;
    queue_index = 0;

#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
    swr = swr_alloc();
#endif
}

void AudioSource::init(void)
{
    position = 0;
    samples_frac = 0;
    queue_index = 0;
#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
    if (swr_is_initialized(swr))
        swr_close(swr);
#endif
}

int AudioSource::nbQueue()
{
    return buffer_queue.size();
}

int AudioSource::nbQueueProcessed()
{
    return queue_index;
}

int AudioSource::queueSize()
{
    int totalSize = 0;
    for (auto& buffer : buffer_queue) {
        totalSize += buffer->sampleSize;
    }
    return totalSize;
}

int AudioSource::getPosition()
{
    int totalPos = 0;
    for (int i=0; i<queue_index; i++) {
        totalPos += buffer_queue[i]->sampleSize;
    }
    totalPos += position;

    return totalPos;
}

void AudioSource::setPosition(int pos)
{
    
    if (looping) {
        pos %= queueSize();
    }

    for (int i=0; i<buffer_queue.size(); i++) {
        AudioBuffer* ab = buffer_queue[i];
        if (pos < ab->sampleSize) {
            /* We set the position in this buffer */
            position = pos;
            samples_frac = 0;
            break;
        }
        else {
            /* We traverse the buffer */
            pos -= ab->sampleSize;
        }
    }
}

int AudioSource::mixWith( struct timespec ticks, uint8_t* outSamples, int outBytes, int outBitDepth, int outNbChannels, int outFrequency, float outVolume)
{
    if (state != SOURCE_PLAYING)
        return -1;

    if (buffer_queue.empty())
        return -1;

    debuglog(LCF_SOUND | LCF_FRAME, "Start mixing source ", id);

    AudioBuffer* curBuf = buffer_queue[queue_index];

#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
    /* Get the sample format */
    AVSampleFormat inFormat, outFormat;
    switch (curBuf->format) {
        case SAMPLE_FMT_U8:
            inFormat = AV_SAMPLE_FMT_U8;
            break;
        case SAMPLE_FMT_S16:
        case SAMPLE_FMT_MSADPCM:
            inFormat = AV_SAMPLE_FMT_S16;
            break;
        case SAMPLE_FMT_S32:
            inFormat = AV_SAMPLE_FMT_S32;
            break;
        case SAMPLE_FMT_FLT:
            inFormat = AV_SAMPLE_FMT_FLT;
            break;
        case SAMPLE_FMT_DBL:
            inFormat = AV_SAMPLE_FMT_DBL;
            break;
        default:
            debuglog(LCF_SOUND | LCF_FRAME | LCF_ERROR, "Unknown sample format");
            break;
    }
    if (outBitDepth == 8)
        outFormat = AV_SAMPLE_FMT_U8;
    if (outBitDepth == 16)
        outFormat = AV_SAMPLE_FMT_S16;

    /* Check if SWR context is initialized.
     * If not, set parameters and init it
     */
    if (! swr_is_initialized(swr)) {
        /* Set channel layout */
        if (curBuf->nbChannels == 1)
            av_opt_set_int(swr, "in_channel_layout", AV_CH_LAYOUT_MONO, 0);
        if (curBuf->nbChannels == 2)
            av_opt_set_int(swr, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
        if (outNbChannels == 1)
            av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
        if (outNbChannels == 2)
            av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);

        /* Set sample format */
        av_opt_set_sample_fmt(swr, "in_sample_fmt", inFormat, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", outFormat, 0);

        /* Set sampling frequency */
        av_opt_set_int(swr, "in_sample_rate", curBuf->frequency, 0);
        av_opt_set_int(swr, "out_sample_rate", outFrequency, 0);

        /* Open the context */
        if (swr_init(swr) < 0) {
            debuglog(LCF_SOUND | LCF_FRAME | LCF_ERROR, "Error initializing swr context");
            return 0;
        }
    }
#endif

    /* Mixing source volume and master volume.
     * Taken from openAL doc:
     * "The implementation is free to clamp the total gain (effective gain
     * per-source multiplied by the listener gain) to one to prevent overflow."
     *
     * TODO: This is where we can support panning.
     */
    float resultVolume = (volume * outVolume) > 1.0?1.0:(volume*outVolume);
    int lvas = (int)(resultVolume * 65536.0f);
    int rvas = (int)(resultVolume * 65536.0f);

    /* Number of samples to advance in the buffer. */
    int inNbSamples = ticksToSamples(ticks, curBuf->frequency);

    int oldPosition = position;
    int newPosition = position + inNbSamples;

    /* Allocate the mixed audio array */
#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
    int outNbSamples = outBytes / (outNbChannels * outBitDepth / 8);
    mixedSamples.resize(outBytes);
    uint8_t* begMixed = mixedSamples.data();
#endif

    int convOutSamples = 0;
    uint8_t* begSamples;
    int availableSamples = curBuf->getSamples(begSamples, inNbSamples, oldPosition);

    if (availableSamples == inNbSamples) {
        /* We did not reach the end of the buffer, easy case */

        position = newPosition;
        debuglog(LCF_SOUND | LCF_FRAME, "  Buffer ", curBuf->id, " in read in range ", oldPosition, " - ", position);
#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
        convOutSamples = swr_convert(swr, &begMixed, outNbSamples, const_cast<const uint8_t**>(&begSamples), inNbSamples);
#endif
    }
    else {
        /* We reached the end of the buffer */
        debuglog(LCF_SOUND | LCF_FRAME, "  Buffer ", curBuf->id, " is read from ", oldPosition, " to its end ", curBuf->sampleSize);
#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
        if (availableSamples > 0)
            swr_convert(swr, nullptr, 0, const_cast<const uint8_t**>(&begSamples), availableSamples);
#endif

        int remainingSamples = inNbSamples - availableSamples;
        if (source == SOURCE_CALLBACK) {
            /* We refill our buffer using the callback function,
             * until we got enough bytes for this frame
             */
            while (remainingSamples > 0) {
                /* Before doing the callback, we must fake that the timer has
                 * advanced by the number of samples already read
                 */
                int64_t extraTicks = static_cast<int64_t>(1000000000) * (-remainingSamples);
                extraTicks /= curBuf->frequency;
                detTimer.fakeAdvanceTimer({extraTicks / 1000000000, extraTicks % 1000000000});
                callback(curBuf);
                detTimer.fakeAdvanceTimer({0, 0});
                availableSamples = curBuf->getSamples(begSamples, remainingSamples, 0);
#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
                    swr_convert(swr, nullptr, 0, const_cast<const uint8_t**>(&begSamples), availableSamples);
#endif
                debuglog(LCF_SOUND | LCF_FRAME, "  Buffer ", curBuf->id, " is read again from 0 to ", availableSamples);
                if (remainingSamples == availableSamples)
                    position = availableSamples;
                remainingSamples -= availableSamples;
            }

#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
            /* Get the mixed samples */
            convOutSamples = swr_convert(swr, &begMixed, outNbSamples, nullptr, 0);
#endif
        }
        else {
            int queue_size = buffer_queue.size();
            int finalIndex;
            int finalPos;

            /* Our for loop conditions are different if we are looping or not */
            if (looping) {
                for (int i=(queue_index+1)%queue_size; remainingSamples>0; i=(i+1)%queue_size) {
                    AudioBuffer* loopbuf = buffer_queue[i];
                    availableSamples = loopbuf->getSamples(begSamples, remainingSamples, 0);
                    debuglog(LCF_SOUND | LCF_FRAME, "  Buffer ", loopbuf->id, " in read in range 0 - ", availableSamples);
#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
                    swr_convert(swr, nullptr, 0, const_cast<const uint8_t**>(&begSamples), availableSamples);
#endif
                    if (remainingSamples == availableSamples) {
                        finalIndex = i;
                        finalPos = availableSamples;
                    }
                    remainingSamples -= availableSamples;
                }
            }
            else {
                for (int i=queue_index+1; (remainingSamples>0) && (i<queue_size); i++) {
                    AudioBuffer* loopbuf = buffer_queue[i];
                    availableSamples = loopbuf->getSamples(begSamples, remainingSamples, 0);
                    debuglog(LCF_SOUND | LCF_FRAME, "  Buffer ", loopbuf->id, " in read in range 0 - ", availableSamples);
#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
                    swr_convert(swr, nullptr, 0, const_cast<const uint8_t**>(&begSamples), availableSamples);
#endif
                    if (remainingSamples == availableSamples) {
                        finalIndex = i;
                        finalPos = availableSamples;
                    }
                    remainingSamples -= availableSamples;
                }
            }

#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)
            /* Get the mixed samples */
            convOutSamples = swr_convert(swr, &begMixed, outNbSamples, nullptr, 0);
#endif

            if (remainingSamples > 0) {
                /* We reached the end of the buffer queue */
                init();
                state = SOURCE_STOPPED;
                debuglog(LCF_SOUND | LCF_FRAME, "  End of the queue reached");
            }
            else {
                /* Update the position in the buffer */
                queue_index = finalIndex;
                position = finalPos;
            }
        }

    }

#if defined(LIBTAS_ENABLE_AVDUMPING) || defined(LIBTAS_ENABLE_SOUNDPLAYBACK)

#define clamptofullsignedrange(x,lo,hi) ((static_cast<unsigned int>((x)-(lo))<=static_cast<unsigned int>((hi)-(lo)))?(x):(((x)<0)?(lo):(hi)))

    /* Add mixed source to the output buffer */
    if (outBitDepth == 8) {
        for (int s=0; s<convOutSamples*outNbChannels; s+=outNbChannels) {
            int myL = mixedSamples[s];
            int otherL = outSamples[s];
            int sumL = otherL + ((myL * lvas) >> 16) - 256;
            outSamples[s] = clamptofullsignedrange(sumL, 0, UINT8_MAX);

            if (outNbChannels == 2) {
                int myR = mixedSamples[s+1];
                int otherR = outSamples[s+1];
                int sumR = otherR + ((myR * rvas) >> 16);
                outSamples[s+1] = clamptofullsignedrange(sumR, 0, UINT8_MAX);
            }
        }
    }

    if (outBitDepth == 16) {
        for (int s=0; s<convOutSamples*outNbChannels; s+=outNbChannels) {
            int myL = reinterpret_cast<int16_t*>(mixedSamples.data())[s];
            int otherL = reinterpret_cast<int16_t*>(outSamples)[s];
            int sumL = otherL + ((myL * lvas) >> 16);
            reinterpret_cast<int16_t*>(outSamples)[s] = clamptofullsignedrange(sumL, INT16_MIN, INT16_MAX);

            if (outNbChannels == 2) {
                int myR = reinterpret_cast<int16_t*>(mixedSamples.data())[s+1];
                int otherR = reinterpret_cast<int16_t*>(outSamples)[s+1];
                int sumR = otherR + ((myR * rvas) >> 16);
                reinterpret_cast<int16_t*>(outSamples)[s+1] = clamptofullsignedrange(sumR, INT16_MIN, INT16_MAX);
            }
        }
    }
#endif

    return convOutSamples;
}

