/*
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2011 Mina Nagy Zaki
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * resampling audio filter
 */

#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libswresample/swresample.h"
#include "avfilter.h"
#include "audio.h"
#include "internal.h"

typedef struct {
    double ratio;
    struct SwrContext *swr;
} AResampleContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    AResampleContext *aresample = ctx->priv;
    int ret = 0;
    char *argd = av_strdup(args);

    aresample->swr = swr_alloc();
    if (!aresample->swr)
        return AVERROR(ENOMEM);

    if (args) {
        char *ptr=argd, *token;

        while(token = av_strtok(ptr, ":", &ptr)) {
            char *value;
            av_strtok(token, "=", &value);

            if(value) {
                if((ret=av_opt_set(aresample->swr, token, value, 0)) < 0)
                    goto end;
            } else {
                int out_rate;
                if ((ret = ff_parse_sample_rate(&out_rate, token, ctx)) < 0)
                    goto end;
                if((ret = av_opt_set_int(aresample->swr, "osr", out_rate, 0)) < 0)
                    goto end;
            }
        }
    }
end:
    av_free(argd);
    return ret;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AResampleContext *aresample = ctx->priv;
    swr_free(&aresample->swr);
}

static int query_formats(AVFilterContext *ctx)
{
    AResampleContext *aresample = ctx->priv;
    int out_rate                   = av_get_int(aresample->swr, "osr", NULL);
    uint64_t out_layout            = av_get_int(aresample->swr, "ocl", NULL);
    enum AVSampleFormat out_format = av_get_int(aresample->swr, "osf", NULL);

    AVFilterLink *inlink  = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];

    AVFilterFormats        *in_formats      = avfilter_all_formats(AVMEDIA_TYPE_AUDIO);
    AVFilterFormats        *out_formats;
    AVFilterFormats        *in_samplerates  = ff_all_samplerates();
    AVFilterFormats        *out_samplerates;
    AVFilterChannelLayouts *in_layouts      = ff_all_channel_layouts();
    AVFilterChannelLayouts *out_layouts;

    avfilter_formats_ref  (in_formats,      &inlink->out_formats);
    avfilter_formats_ref  (in_samplerates,  &inlink->out_samplerates);
    ff_channel_layouts_ref(in_layouts,      &inlink->out_channel_layouts);

    if(out_rate > 0) {
        out_samplerates = avfilter_make_format_list((int[]){ out_rate, -1 });
    } else {
        out_samplerates = ff_all_samplerates();
    }
    avfilter_formats_ref(out_samplerates, &outlink->in_samplerates);

    if(out_format != AV_SAMPLE_FMT_NONE) {
        out_formats = avfilter_make_format_list((int[]){ out_format, -1 });
    } else
        out_formats = avfilter_make_all_formats(AVMEDIA_TYPE_AUDIO);
    avfilter_formats_ref(out_formats, &outlink->in_formats);

    if(out_layout) {
        out_layouts = avfilter_make_format64_list((int64_t[]){ out_layout, -1 });
    } else
        out_layouts = ff_all_channel_layouts();
    ff_channel_layouts_ref(out_layouts, &outlink->in_channel_layouts);

    return 0;
}


static int config_output(AVFilterLink *outlink)
{
    int ret;
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    AResampleContext *aresample = ctx->priv;
    int out_rate;
    uint64_t out_layout;
    enum AVSampleFormat out_format;

    aresample->swr = swr_alloc_set_opts(aresample->swr,
                                        outlink->channel_layout, outlink->format, outlink->sample_rate,
                                        inlink->channel_layout, inlink->format, inlink->sample_rate,
                                        0, ctx);
    if (!aresample->swr)
        return AVERROR(ENOMEM);

    ret = swr_init(aresample->swr);
    if (ret < 0)
        return ret;

    out_rate   = av_get_int(aresample->swr, "osr", NULL);
    out_layout = av_get_int(aresample->swr, "ocl", NULL);
    out_format = av_get_int(aresample->swr, "osf", NULL);
    outlink->time_base = (AVRational) {1, out_rate};

    av_assert0(outlink->sample_rate == out_rate);
    av_assert0(outlink->channel_layout == out_layout);
    av_assert0(outlink->format == out_format);

    aresample->ratio = (double)outlink->sample_rate / inlink->sample_rate;

    av_log(ctx, AV_LOG_INFO, "r:%"PRId64"Hz -> r:%"PRId64"Hz\n",
           inlink->sample_rate, outlink->sample_rate);
    return 0;
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamplesref)
{
    AResampleContext *aresample = inlink->dst->priv;
    const int n_in  = insamplesref->audio->nb_samples;
    int n_out       = n_in * aresample->ratio;
    AVFilterLink *const outlink = inlink->dst->outputs[0];
    AVFilterBufferRef *outsamplesref = ff_get_audio_buffer(outlink, AV_PERM_WRITE, n_out);

    n_out = swr_convert(aresample->swr, outsamplesref->data, n_out,
                                 (void *)insamplesref->data, n_in);

    avfilter_copy_buffer_ref_props(outsamplesref, insamplesref);
    outsamplesref->audio->sample_rate = outlink->sample_rate;
    outsamplesref->audio->nb_samples  = n_out;
    outsamplesref->pts = insamplesref->pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
        av_rescale_q(insamplesref->pts, inlink->time_base, outlink->time_base);

    ff_filter_samples(outlink, outsamplesref);
    avfilter_unref_buffer(insamplesref);
}

AVFilter avfilter_af_aresample = {
    .name          = "aresample",
    .description   = NULL_IF_CONFIG_SMALL("Resample audio data."),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(AResampleContext),

    .inputs    = (const AVFilterPad[]) {{ .name      = "default",
                                    .type            = AVMEDIA_TYPE_AUDIO,
                                    .filter_samples  = filter_samples,
                                    .min_perms       = AV_PERM_READ, },
                                  { .name = NULL}},
    .outputs   = (const AVFilterPad[]) {{ .name      = "default",
                                    .config_props    = config_output,
                                    .type            = AVMEDIA_TYPE_AUDIO, },
                                  { .name = NULL}},
};
