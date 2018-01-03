#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_codec_module.h"

int BUFFER_SIZE = 500;

void ngx_rtmp_live_start(ngx_rtmp_session_t *s);
static void buffer_send(ngx_rtmp_session_t *s, ngx_rtmp_live_ctx_t *pctx, int end);

struct bufitem {
    ngx_chain_t *pkt;
    int kf;
    uint32_t  csid;
    uint32_t  timestamp;
    uint32_t  mlen;
    uint8_t   type;
    uint32_t  msid;
    ngx_rtmp_core_srv_conf_t *cscf;
    uint32_t current_time;
};

void buffer_alloc(ngx_rtmp_session_t *s) {
    printf("buffer: alloc\n");
    void **buffer = malloc(BUFFER_SIZE*sizeof(void *));
    for (int i=0; i<=BUFFER_SIZE; i++) {
        buffer[i] = NULL;
    }
    s->buffer = buffer;
}
int buffer_find_next(ngx_rtmp_session_t *s) {
    int next = s->buffer_i+1;
    if (next >= BUFFER_SIZE) {
        return 0;
    } else {
        return next;
    }
}

int buffer_add(ngx_rtmp_session_t *s, struct bufitem *i) {
    int next = buffer_find_next(s);
    //printf(" buffer: add %d\n", next);
    if (i->kf == 1) printf("buffer: keyframe!\n");
    s->buffer[next] = i;
    s->buffer_i = next;

    return next;
}

static ngx_int_t
buffer_ngx_rtmp_live_av(ngx_rtmp_session_t *s, struct bufitem *h, ngx_rtmp_live_ctx_t *pctx)
{

    ngx_chain_t *in = h->pkt;

    ngx_rtmp_live_ctx_t            *ctx;
    ngx_rtmp_codec_ctx_t           *codec_ctx;
    ngx_chain_t                    *header, *coheader, *meta,
                                   *apkt, *aapkt, *acopkt, *rpkt;
    ngx_rtmp_core_srv_conf_t       *cscf;
    ngx_rtmp_live_app_conf_t       *lacf;
    ngx_rtmp_session_t             *ss;
    ngx_rtmp_header_t               ch, lh, clh;
    ngx_int_t                       rc, mandatory, dummy_audio;
    ngx_uint_t                      prio;
    ngx_uint_t                      peers;
    ngx_uint_t                      meta_version;
    ngx_uint_t                      csidx;
    uint32_t                        delta;
    ngx_rtmp_live_chunk_stream_t   *cs;
#ifdef NGX_DEBUG
    const char                     *type_s;

    type_s = (h->type == NGX_RTMP_MSG_VIDEO ? "video" : "audio");
#endif

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    if (lacf == NULL) {
        return NGX_ERROR;
    }

    if (!lacf->live || in == NULL  || in->buf == NULL) {
        return NGX_OK;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx == NULL || ctx->stream == NULL) {
        return NGX_OK;
    }

    if (ctx->publishing == 0) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "live: %s from non-publisher", type_s);
        return NGX_OK;
    }

    if (!ctx->stream->active) {
        ngx_rtmp_live_start(s);
    }

    if (ctx->idle_evt.timer_set) {
        ngx_add_timer(&ctx->idle_evt, lacf->idle_timeout);
    }

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "live: %s packet timestamp=%uD",
            type_s, h->timestamp);

    peers = 0;
    apkt = NULL;
    aapkt = NULL;
    acopkt = NULL;
    header = NULL;
    coheader = NULL;
    meta = NULL;
    meta_version = 0;
    mandatory = 0;

    prio = (h->type == NGX_RTMP_MSG_VIDEO ?
            ngx_rtmp_get_video_frame_type(in) : 0);

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    struct bufitem *i = malloc(sizeof(struct bufitem));
    i->current_time = s->current_time;
    i->csid = h->csid;
    i->timestamp = h->timestamp;
    i->mlen = h->mlen;
    i->type = h->type;
    i->msid = h->msid;
    i->pkt = ngx_rtmp_append_shared_bufs(cscf, NULL, in);
    i->cscf = cscf;

    if (prio == NGX_RTMP_VIDEO_KEY_FRAME) {
        i->kf = 1;
    } else {
        i->kf = 0;
    }

    
    csidx = !(lacf->interleave || h->type == NGX_RTMP_MSG_VIDEO);

    cs  = &ctx->cs[csidx];

    ngx_memzero(&ch, sizeof(ch));

    ch.timestamp = h->timestamp;
    ch.msid = NGX_RTMP_MSID;
    ch.csid = cs->csid;
    ch.type = h->type;

    lh = ch;

    if (cs->active) {
        lh.timestamp = cs->timestamp;
    }

    clh = lh;
    clh.type = (h->type == NGX_RTMP_MSG_AUDIO ? NGX_RTMP_MSG_VIDEO :
            NGX_RTMP_MSG_AUDIO);

    cs->active = 1;
    cs->timestamp = ch.timestamp;

    delta = ch.timestamp - lh.timestamp;
    /*
       if (delta >> 31) {
       ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
       "live: clipping non-monotonical timestamp %uD->%uD",
       lh.timestamp, ch.timestamp);

       delta = 0;

       ch.timestamp = lh.timestamp;
       }
       */
    rpkt = ngx_rtmp_append_shared_bufs(cscf, NULL, in);

    ngx_rtmp_prepare_message(s, &ch, &lh, rpkt);

    codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    if (codec_ctx) {

        if (h->type == NGX_RTMP_MSG_AUDIO) {
            header = codec_ctx->aac_header;

            if (lacf->interleave) {
                coheader = codec_ctx->avc_header;
            }

            if (codec_ctx->audio_codec_id == NGX_RTMP_AUDIO_AAC &&
                    ngx_rtmp_is_codec_header(in))
            {
                prio = 0;
                mandatory = 1;
            }

        } else {
            header = codec_ctx->avc_header;

            if (lacf->interleave) {
                coheader = codec_ctx->aac_header;
            }

            if (codec_ctx->video_codec_id == NGX_RTMP_VIDEO_H264 &&
                    ngx_rtmp_is_codec_header(in))
            {
                prio = 0;
                mandatory = 1;
            }
        }

        if (codec_ctx->meta) {
            meta = codec_ctx->meta;
            meta_version = codec_ctx->meta_version;
        }
    }



    for (int i=0; i<=0; i++) {


        if (pctx == ctx || pctx->paused) {
            continue;
        }

        ss = pctx->session;
        cs = &pctx->cs[csidx];

        /* send metadata */

        if (meta && meta_version != pctx->meta_version) {
            ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                    "live: meta");

            if (ngx_rtmp_send_message(ss, meta, 0) == NGX_OK) {
                pctx->meta_version = meta_version;
            }
        }


        /* sync stream */

        if (cs->active && (lacf->sync && cs->dropped > lacf->sync)) {
            ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                    "live: sync %s dropped=%uD", type_s, cs->dropped);

            cs->active = 0;
            cs->dropped = 0;
        }


        /* absolute packet */

        if (!cs->active) {

            if (mandatory) {
                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                        "live: skipping header");
                continue;
            }

            if (lacf->wait_video && h->type == NGX_RTMP_MSG_AUDIO &&
                    !pctx->cs[0].active)
            {
                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                        "live: waiting for video");
                continue;
            }

            if (lacf->wait_key && prio != NGX_RTMP_VIDEO_KEY_FRAME &&
                    (lacf->interleave || h->type == NGX_RTMP_MSG_VIDEO))
            {
                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                        "live: skip non-key");
                continue;
            }

            dummy_audio = 0;
            if (lacf->wait_video && h->type == NGX_RTMP_MSG_VIDEO &&
                    !pctx->cs[1].active)
            {
                dummy_audio = 1;
                if (aapkt == NULL) {
                    aapkt = ngx_rtmp_alloc_shared_buf(cscf);
                    ngx_rtmp_prepare_message(s, &clh, NULL, aapkt);
                }
            }

            if (header || coheader) {

                /* send absolute codec header */

                ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                        "live: abs %s header timestamp=%uD",
                        type_s, lh.timestamp);

                if (header) {
                    if (apkt == NULL) {
                        apkt = ngx_rtmp_append_shared_bufs(cscf, NULL, header);
                        ngx_rtmp_prepare_message(s, &lh, NULL, apkt);
                    }
                    rc = ngx_rtmp_send_message(ss, apkt, 0);
                    if (rc != NGX_OK) {
                        continue;
                    }
                }

                if (coheader) {
                    if (acopkt == NULL) {
                        acopkt = ngx_rtmp_append_shared_bufs(cscf, NULL, coheader);
                        ngx_rtmp_prepare_message(s, &clh, NULL, acopkt);
                    }

                    rc = ngx_rtmp_send_message(ss, acopkt, 0);
                    if (rc != NGX_OK) {
                        continue;
                    }

                } else if (dummy_audio) {
                    ngx_rtmp_send_message(ss, aapkt, 0);
                }

                cs->timestamp = lh.timestamp;
                cs->active = 1;
                ss->current_time = cs->timestamp;

            } else {

                /* send absolute packet */

                ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                        "live: abs %s packet timestamp=%uD",
                        type_s, ch.timestamp);

                if (apkt == NULL) {
                    apkt = ngx_rtmp_append_shared_bufs(cscf, NULL, in);
                    ngx_rtmp_prepare_message(s, &ch, NULL, apkt);
                }
                rc = ngx_rtmp_send_message(ss, apkt, prio);
                if (rc != NGX_OK) {
                    continue;
                }

                cs->timestamp = ch.timestamp;
                cs->active = 1;
                ss->current_time = cs->timestamp;

                ++peers;

                if (dummy_audio) {

                    ngx_rtmp_send_message(ss, aapkt, 0);
                }

                continue;
            }
        }

        /* send relative packet */

        ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                "live: rel %s packet delta=%uD",
                type_s, delta);

        if (ngx_rtmp_send_message(ss, rpkt, prio) != NGX_OK) {
            ++pctx->ndropped;

            cs->dropped += delta;

            if (mandatory) {
                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                        "live: mandatory packet failed");
                ngx_rtmp_finalize_session(ss);
            }

            continue;
        }

        cs->timestamp += delta;
        ++peers;
        ss->current_time = cs->timestamp;

    }

    if (rpkt) {
        ngx_rtmp_free_shared_chain(cscf, rpkt);
    }

    if (apkt) {
        ngx_rtmp_free_shared_chain(cscf, apkt);
    }

    if (aapkt) {
        ngx_rtmp_free_shared_chain(cscf, aapkt);
    }

    if (acopkt) {
        ngx_rtmp_free_shared_chain(cscf, acopkt);
    }

    ngx_rtmp_update_bandwidth(&ctx->stream->bw_in, h->mlen);
    ngx_rtmp_update_bandwidth(&ctx->stream->bw_out, h->mlen * peers);

    ngx_rtmp_update_bandwidth(h->type == NGX_RTMP_MSG_AUDIO ?
            &ctx->stream->bw_in_audio :
            &ctx->stream->bw_in_video,
            h->mlen);

    return NGX_OK;
}





static void buffer_send(ngx_rtmp_session_t *s, ngx_rtmp_live_ctx_t *ctx, int end) {

    printf("buffer: send called (end %d)\n", end);
    ctx->buffer_sent = 1;

    int start = -1;
    int i;
    for (i=end; i>=0; i--) {

        struct bufitem *pkt = s->buffer[i];
        if (pkt != NULL && pkt->kf == 1) {
            start = i;
            break;
        }
    }
    if (start == -1) {
        for (i=BUFFER_SIZE; i>end; i--) {

            struct bufitem *pkt = s->buffer[i];
            if (pkt != NULL && pkt->kf == 1) {
                start = i;
                break;
            }
        }
    }

    if (start == -1) {
        printf("buffer: no key frame in buffer!\n");
        return;
    }

    if (start >= BUFFER_SIZE) {
        start = 0;
    }
    //int end = s->buffer_i-1;
    if (end < 0) {
        end = BUFFER_SIZE;
    }

    struct bufitem *pkt = s->buffer[start];
    if (pkt == NULL || (start==0 && end==0)) {
        printf("No buffer!\n");
        return;
    }

    printf("buffer: about to send buffer from %d to %d (excluding)\n", start, end);

    while (pkt != NULL) {

        printf("buffer: sending %d\n", start);
        buffer_ngx_rtmp_live_av(s, pkt, ctx);

        start++;

        if (start == BUFFER_SIZE) {
            start = 0;
        }

        if (start == end) break;

        pkt = s->buffer[start];
    }
}



