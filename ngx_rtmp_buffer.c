#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<stdlib.h>
#include<unistd.h>

#include <ngx_config.h>
#include <ngx_core.h>

#include "ngx_rtmp_bufshared.h"
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_codec_module.h"
#include "ngx_rtmp_timestamp_fix.h"

struct bufreceiver {
    ngx_rtmp_session_t              *s;
    struct bufreceiver              *next;
};

struct bufitem {
    ngx_chain_t                     *pkt;
    ngx_rtmp_header_t               *h;
    ngx_rtmp_live_chunk_stream_t    *cs;
    int                             *kf;
    uint32_t                        *delta;
    ngx_rtmp_header_t               *ch, *lh, *clh;
};

pthread_mutex_t lock;

void buffer_init() {
    root_bufstr = NULL;
    pthread_mutex_init(&lock, NULL);
}

void ngx_rtmp_live_start(ngx_rtmp_session_t *s);
static ngx_int_t buffer_send(ngx_rtmp_session_t *s, struct bufitem *bi, ngx_rtmp_live_ctx_t *pctx);

void bufstr_remove (char *name) {

    pthread_mutex_lock(&lock);

    bufstr *last = root_bufstr;
    bufstr *cur = root_bufstr;

    while (cur != NULL) {
        if (strcmp((char *) name, (char *) cur->name) == 0) {
            last->next = cur->next;
            break;
        }
        last = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&lock);
}

void bufstr_upsert (char *name, ngx_rtmp_session_t *s) {
    pthread_mutex_lock(&lock);
    bufstr *last = root_bufstr;
    bufstr *cur = root_bufstr;

    while (cur != NULL) {
        if (strcmp(name, cur->name) == 0) {
            break;
        }
        last = cur;
        cur = cur->next;
    }

    if (root_bufstr == NULL) {

        printf("buffer: new session '%s' pid %d\n", name, getpid());
        root_bufstr = malloc(sizeof(bufstr));
        root_bufstr->name = name;
        root_bufstr->s = s;
        root_bufstr->next = NULL;
        root_bufstr->r = NULL;

    } else if (cur == NULL) {

        printf("buffer: new session '%s' pid %d\n", name, getpid());

        bufstr *nw;
        nw = malloc(sizeof(bufstr));

        nw->name = name;
        nw->s = s;
        nw->next = NULL;
        nw->r = NULL;
        last->next = nw;

    } else {
        printf("buffer: update session '%s' pid %d\n", name, getpid());
        char *oldname = cur->name;
        cur->name = name;
        cur->s = s;
        free(oldname);
        printf("buffer: updated\n");
    }
    pthread_mutex_unlock(&lock);
}

bufstr *bufstr_get (char *name) {
    pthread_mutex_lock(&lock);
    bufstr *cur = root_bufstr;
    while (cur != NULL) {
        if (strcmp(name,cur->name) == 0) {
            pthread_mutex_unlock(&lock);
            //printf("buffer: got %s\n", name);
            return cur;
        }
        cur = cur->next;
    }
    printf("buffer: not found '%s' pid %d\n", name, getpid());
    pthread_mutex_unlock(&lock);
    return NULL;
}

void buffer_publisher_free (char *name, ngx_rtmp_session_t *r) {
    bufstr *bp = bufstr_get(name);
    pthread_mutex_lock(&lock);
    if (bp == NULL) return;

    struct bufreceiver *cur = bp->r;
    struct bufreceiver *last = bp->r;

    while (cur != NULL) {
        if (strcmp(cur->s->name, r->name) == 0) {
            printf("buffer: removing subscriber '%s' from: '%s'\n", cur->s->name, name);
            last->next = cur->next;
            if (cur == bp->r) {
                bp->r = cur->next;
            }
            free(cur);
            break;
        }
        last = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&lock);
}


void buffer_publisher_register (ngx_rtmp_session_t *p, ngx_rtmp_session_t *r) {
    bufstr *bp = NULL;
    if (p && p->name) {
        printf("searching %s\n", p->name);
        bp = bufstr_get(p->name);
    } else {
        return;
    }
    if (bp == NULL) {
        printf("cannot find %s\n", p->name);
        return;
    } else {
        printf("found!\n");
    }

    printf("buffer: registering subscriber '%s' to '%s'\n", r->name, p->name);
    pthread_mutex_lock(&lock);
    struct bufreceiver *cur = bp->r;
    struct bufreceiver *last = bp->r;

    while (cur != NULL) {
        last = cur;
        cur = cur->next;
    }

    struct bufreceiver *n = malloc(sizeof(struct bufreceiver));
    n->s = r;
    n->next = NULL;

    if (last == NULL) {
        printf("nw\n");
        bp->r = n;
    } else {
        last->next = n;
    }
    pthread_mutex_unlock(&lock);

}

void buffer_reset_buffer_i (ngx_rtmp_session_t *s) {

    bufstr *bs = bufstr_get(s->name);
    struct bufreceiver *cur = bs->r;

    while (cur != NULL) {
        bufstr *br = bufstr_get(cur->s->name);
        printf("buffer: pointer reset %s %d > %d\n", cur->s->name, *br->buffer_i, *bs->buffer_i);

        pthread_mutex_lock(&lock);
        *br->buffer_i = *bs->buffer_i;
        ngx_rtmp_live_start(br->s);
        cur = cur->next;

        pthread_mutex_unlock(&lock);
    }
}

int *buffer_is_full(ngx_rtmp_session_t *s) {
    bufstr *b = bufstr_get(s->name);

    if (*b->buffer_is_full == 1)
        return b->buffer_is_full;

    int i;
    for (i=0; i<b->buffer_size; i=i+1) {
        if (b->buffer[i] == NULL) {
            *b->buffer_is_full = 0;
            return b->buffer_is_full;
        }
    }

    printf("buffer: filled!\n");
    *b->buffer_is_full = 1;
    return b->buffer_is_full;
}

void buffer_bufitem_free(ngx_rtmp_session_t *s, struct bufitem *prev) {

    pthread_mutex_lock(&lock);
    if (prev != NULL) {

        if (prev->pkt != NULL)
            ngx_rtmp_free_shared_chain(
                    ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module),
                    prev->pkt
                    );

        if (prev->pkt != NULL)
            free(prev->kf);
        if (prev->delta != NULL)
            free(prev->delta);
        if (prev->ch != NULL)
            free(prev->ch);
        if (prev->lh != NULL)
            free(prev->lh);
        if (prev->clh != NULL)
            free(prev->clh);
        if (prev->h != NULL)
            free(prev->h);
        if (prev->cs != NULL)
            free(prev->cs);

        free(prev);
    }

    pthread_mutex_unlock(&lock);

}

void buffer_free(ngx_rtmp_session_t *s) {

    bufstr *b = bufstr_get(s->name);
    printf("buffer: free buffer %s\n", s->name);
    bufstr_remove(s->name);

    ngx_rtmp_live_app_conf_t *lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    b->buffer_size = (int)lacf->kfbuflen;



    printf("buffer: freeing buffer\n");
    if (b->buffer != NULL) {
        int i;
        for (i=0; i<=b->buffer_size; i=i+1) {
            buffer_bufitem_free(s, b->buffer[i]);
        }
    }

    pthread_mutex_lock(&lock);
    printf("buffer: freeing vars\n");
    if (b->name != NULL)
        free(b->name);
    if (b->buffer != NULL)
        free(b->buffer);
    if (b->buffer_is_full != NULL)
        free(b->buffer_is_full);
    if (b->buffer_is_allocated != NULL)
        free(b->buffer_is_allocated);
    if (b->buffer_i != NULL)
        free(b->buffer_i);
    if (b->buffer_was_bursted != NULL)
        free(b->buffer_was_bursted);

    printf("buffer: freeing obj\n");
    free(b);


    pthread_mutex_unlock(&lock);
    //printf("buffer: freeing name in rtmp session\n");
    //free(s->name);

    printf("buffer: freed\n");
}

struct bufitem *buffer_bufitem_alloc() {
    struct bufitem *i = malloc(sizeof(struct bufitem));
    i->pkt = NULL;
    i->kf = malloc(sizeof(int));
    i->delta = malloc(sizeof(uint32_t));
    i->ch = malloc(sizeof(ngx_rtmp_header_t));
    i->lh = malloc(sizeof(ngx_rtmp_header_t));
    i->clh = malloc(sizeof(ngx_rtmp_header_t));
    i->h = malloc(sizeof(ngx_rtmp_header_t));
    i->cs = malloc(sizeof(ngx_rtmp_live_chunk_stream_t));
    return i;
}

void buffer_alloc(ngx_rtmp_session_t *s) {

    bufstr *b = bufstr_get(s->name);
    ngx_rtmp_live_app_conf_t *lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    b->buffer_size = (int)lacf->kfbuflen;

    printf("buffer: alloc key frame buffer %d\n", b->buffer_size);

    pthread_mutex_lock(&lock);
    b->buffer = malloc(b->buffer_size*sizeof(struct bufitem));

    int i;
    for (i=0; i<=b->buffer_size; i=i+1) {
        b->buffer[i] = NULL;
    }

    b->buffer_is_allocated = malloc(sizeof(int));
    b->buffer_is_full = malloc(sizeof(int));
    b->buffer_i = malloc(sizeof(int));
    b->buffer_was_bursted = malloc(sizeof(int));

    *b->buffer_is_allocated = 0;
    *b->buffer_is_full = 0;
    *b->buffer_i = 0;
    *b->buffer_was_bursted = 0;

    pthread_mutex_unlock(&lock);
    printf("buffer: allocated\n");
}

int *buffer_find_next(ngx_rtmp_session_t *s) {
    bufstr *b = bufstr_get(s->name);

    int *next = malloc(sizeof(int));

    *next = *b->buffer_i+1;
    if (*next >= b->buffer_size) {
        *next = 0;
    }


    return next;
}


int *buffer_find_kf_next2(ngx_rtmp_session_t *s) {

    bufstr *b = bufstr_get(s->name);
    struct bufitem *pkt;

    int *start = malloc(sizeof(int));
    int i;
    int end;
    int kfc;

    *start = -1;
    end = *b->buffer_i;
    kfc = 0;

    for (i=end; i>=0; i=i-1) {

        pkt = b->buffer[i];

        if (pkt != NULL && *pkt->kf==1) kfc = kfc+1;
        if (kfc==2 && pkt != NULL && *pkt->kf == 1) {
            *start = i;
            break;
        }
    }
    if (*start == -1) {
        for (i=b->buffer_size; i>end; i=i-1) {

            pkt = b->buffer[i];
            if (pkt != NULL && *pkt->kf==1) kfc = kfc+1;
            if (kfc==2 && pkt != NULL && *pkt->kf == 1) {
                *start = i;
                break;
            }
        }
    }

    if (*start == -1) {
        printf("buffer: no key frame in buffer!\n");
        return NULL;
    }

    if (*start >= b->buffer_size) {
        *start = 0;
    }

    printf("buffer: new client, sending from %d\n", *start);
    return start;
}



int *buffer_find_kf_next(ngx_rtmp_session_t *s) {

    bufstr *b = bufstr_get(s->name);
    struct bufitem *pkt;

    pthread_mutex_lock(&lock);
    int *start = malloc(sizeof(int));
    int i;
    int end;

    *start = -1;
    end = *b->buffer_i;

    for (i=end; i>=0; i=i-1) {

        pkt = b->buffer[i];
        if (pkt != NULL && *pkt->kf == 1) {
            *start = i;
            break;
        }
    }
    if (*start == -1) {
        for (i=b->buffer_size; i>end; i=i-1) {

            pkt = b->buffer[i];
            if (pkt != NULL && *pkt->kf == 1) {
                *start = i;
                break;
            }
        }
    }

    if (*start == -1) {
        printf("buffer: no key frame in buffer!\n");

        *start = *b->buffer_i;

        pthread_mutex_unlock(&lock);
        return start;
    }

    if (*start >= b->buffer_size) {
        *start = 0;
    }


    pthread_mutex_unlock(&lock);
    printf("buffer: new client, sending from %d\n", *start);
    return start;
}

char *buffer_get_pointer_as_string(void *p) {
    char *s = malloc(100*sizeof(char));
    sprintf(s, "%p", p);
    return s;
}

char *get_name_from_v(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v) {


    char *app = malloc(sizeof(char)*(s->app.len+1));
    strncpy(app, (char *)s->app.data, s->app.len);
    app[s->app.len] = '\0';
    char *name = malloc(sizeof(char)*(strlen((char *)v->name)+strlen(app)+2));
    strcpy(name, app);
    strcat(name, "/");
    strcat(name,(char *)v->name);
    return name;

}

void buffer_add(ngx_rtmp_session_t *s, struct bufitem *i) {

    bufstr *b = bufstr_get(s->name);
    struct bufitem *prev;

    int *next = buffer_find_next(s);

    if (*i->kf == 1)
        printf("buffer: keyframe!\n");

    pthread_mutex_lock(&lock);
    prev = b->buffer[*next];
    b->buffer[*next] = i;
    *b->buffer_i = *next;

    pthread_mutex_unlock(&lock);

    free(next);

    // free
    if (prev != NULL && prev->pkt != NULL) {
        buffer_bufitem_free(s, prev);
    }

}

int *buffer_get_cur(ngx_rtmp_session_t *publisher, ngx_rtmp_session_t *receiver) {
    bufstr *br = bufstr_get(receiver->name);
    int *next;
    if (*br->buffer_i == -1) {
        ngx_rtmp_live_app_conf_t *lacf = ngx_rtmp_get_module_app_conf(publisher, ngx_rtmp_live_module);
        if (lacf->key_frame_burst_kf2) {
            next = buffer_find_kf_next2(publisher);
        } else {
            next = buffer_find_kf_next(publisher);
        }
    } else {
        next = buffer_find_next(receiver);
    }
    *br->buffer_i = *next;
    free(next);
    return br->buffer_i;
}


static void buffer_burst(ngx_rtmp_session_t *s, ngx_rtmp_live_ctx_t *pctx)
{
    bufstr *b = bufstr_get(s->name);
    bufstr *br = bufstr_get(pctx->session->name);
    struct bufitem *bi;

    *br->buffer_was_bursted = 1;

    int *start = buffer_get_cur(s, pctx->session);
    int end;
    int i;
    int tend;
    int otherhalf;

    pthread_mutex_lock(&lock);

    *start=*start-1;
    if (*start == -1) 
        *start = b->buffer_size;

    end = *b->buffer_i;
    tend = end;
    otherhalf = -1;

    printf("buffer: bursting %d-%d\n", *start, end);

    if (tend < *start) {
        tend = b->buffer_size;
        otherhalf = end;
    }

    //ngx_rtmp_live_start(br->s);

    for (i = *start; i<tend; i=i+1) {
        bi = b->buffer[i];
        *br->buffer_i = i;

        buffer_send(s, bi, pctx);
    }

    if (otherhalf != -1) {
        for (i = 0; i<otherhalf; i=i+1) {
            bi = b->buffer[i];
            *br->buffer_i = i;
            buffer_send(s, bi, pctx);
        }
    }


    pthread_mutex_unlock(&lock);

    ngx_rtmp_live_start(br->s);

}


static ngx_int_t buffer_send(ngx_rtmp_session_t *s, struct bufitem *bi, ngx_rtmp_live_ctx_t *pctx)
{

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


    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    if (lacf == NULL) {
        return NGX_ERROR;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx == NULL || ctx->stream == NULL) {
        return NGX_OK;
    }

    ngx_chain_t *in = bi->pkt;
    ngx_rtmp_header_t *h = bi->h;
#ifdef NGX_DEBUG
    const char                     *type_s;

    type_s = (h->type == NGX_RTMP_MSG_VIDEO ? "video" : "audio");
#endif


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

    csidx = !(lacf->interleave || h->type == NGX_RTMP_MSG_VIDEO);

    rpkt = ngx_rtmp_append_shared_bufs(cscf, NULL, in);

    delta = *bi->delta;
    ch = *bi->ch;
    lh = *bi->lh;
    clh = *bi->clh;

    //bufstr *b = bufstr_get(pctx->session->name);

    //printf("%s %d %u %u\n",  pctx->session->name,b->buffer_i, ch.timestamp, delta);

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
            return NGX_OK;
        }

        if (lacf->wait_video && h->type == NGX_RTMP_MSG_AUDIO &&
                !pctx->cs[0].active)
        {
            ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                    "live: waiting for video");
            return NGX_OK;
        }

        if (lacf->wait_key && prio != NGX_RTMP_VIDEO_KEY_FRAME &&
                (lacf->interleave || h->type == NGX_RTMP_MSG_VIDEO))
        {
            ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                    "live: skip non-key");
            return NGX_OK;
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
                    return NGX_OK;
                }
            }

            if (coheader) {
                if (acopkt == NULL) {
                    acopkt = ngx_rtmp_append_shared_bufs(cscf, NULL, coheader);
                    ngx_rtmp_prepare_message(s, &clh, NULL, acopkt);
                }

                rc = ngx_rtmp_send_message(ss, acopkt, 0);
                if (rc != NGX_OK) {
                    return NGX_OK;
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
                return NGX_OK;
            }

            cs->timestamp = ch.timestamp;
            cs->active = 1;
            ss->current_time = cs->timestamp;

            ++peers;

            if (dummy_audio) {

                ngx_rtmp_send_message(ss, aapkt, 0);
            }

            return NGX_OK;
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

        return NGX_OK;
    }

    cs->timestamp += delta;
    ++peers;
    ss->current_time = cs->timestamp;


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

    return NGX_OK;


}

ngx_int_t buffer_ngx_rtmp_live_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
        ngx_chain_t *in)
{


    struct bufitem *bi;
    struct bufitem *i;

    ngx_rtmp_core_srv_conf_t       *cscf;
    ngx_uint_t                      prio;
    ngx_rtmp_live_ctx_t            *ctx, *pctx;
    ngx_rtmp_live_app_conf_t       *lacf;
    ngx_rtmp_live_chunk_stream_t   *cs;
    ngx_uint_t                      csidx;

    ngx_rtmp_header_t               ch, lh, clh;
    uint32_t                        delta;

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    if (lacf == NULL) {
        return NGX_ERROR;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx == NULL || ctx->stream == NULL) {
        return NGX_OK;
    }

    if (ctx->publishing == 0) {
        return NGX_OK;
    }

    if (!ctx->stream->active) {
        ngx_rtmp_live_start(s);
    }

    if (ctx->idle_evt.timer_set) {
        ngx_add_timer(&ctx->idle_evt, lacf->idle_timeout);
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);

    prio = (h->type == NGX_RTMP_MSG_VIDEO ?
            ngx_rtmp_get_video_frame_type(in) : 0);
    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    /**
     * timestamp fix start
     */
    uint32_t *off = get_offset(s->name, &s->current_time);
    if (off != NULL)
        h->timestamp = h->timestamp + *off;
    //free(off);
    /**
     * timestamp fix end
     */

    s->current_time = h->timestamp;
    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);

    csidx = !(lacf->interleave || h->type == NGX_RTMP_MSG_VIDEO);
    cs  = &ctx->cs[csidx];

    i = buffer_bufitem_alloc();
    i->pkt = ngx_rtmp_append_shared_bufs(cscf, NULL, in);

    if (prio == NGX_RTMP_VIDEO_KEY_FRAME) {
        *i->kf = 1;
    } else {
        *i->kf = 0;
    }

    i->h->csid = h->csid;
    i->h->timestamp = h->timestamp;
    i->h->mlen = h->mlen;
    i->h->type = h->type;
    i->h->msid = h->msid;

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


    i->cs->active = cs->active;
    i->cs->timestamp = cs->timestamp;
    i->cs->csid = cs->csid;
    i->cs->dropped = cs->dropped;

    *i->delta = delta;
    *i->ch = ch;
    *i->lh = lh;
    *i->clh = clh;

    buffer_add(s, i);
    bufstr *b = bufstr_get(s->name);

    int *full = buffer_is_full(s);
    if (!*full) {
        if (*b->buffer_i == 1)
            printf("buffer: buffer not yet filled\n");
        return NGX_OK;
    } else {

        for (pctx = ctx->stream->ctx; pctx; pctx = pctx->next) {

            if (pctx == ctx || pctx->paused) {
                continue;
            }
            int *start = buffer_get_cur(s, pctx->session);
            bi = b->buffer[*start];

            //disabled
            bufstr *br = bufstr_get(pctx->session->name);
            if (lacf->kfburst == 1 && *br->buffer_was_bursted == 0) {
                buffer_burst(s, pctx);
                continue;
            }

            buffer_send(s, bi, pctx);
        }

    }

    return NGX_OK;
}




