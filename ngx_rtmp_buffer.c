#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_codec_module.h"

int BUFFER_SIZE = 500;

struct bufitem {
	ngx_chain_t *pkt;
	ngx_rtmp_header_t *h;
	ngx_rtmp_live_chunk_stream_t   *cs;
	int kf;
	uint32_t                        delta;
	ngx_rtmp_header_t               ch, lh, clh;
};

void ngx_rtmp_live_start(ngx_rtmp_session_t *s);
static ngx_int_t buffer_send(ngx_rtmp_session_t *s, struct bufitem *bi, ngx_rtmp_live_ctx_t *pctx);

int buffer_is_full(ngx_rtmp_session_t *s) {
	if (s->buffer_is_full == 1)
		return s->buffer_is_full;

	for (int i=0; i<BUFFER_SIZE; i++) {
		if (s->buffer[i] == NULL) {
			s->buffer_is_full = 0;
			return s->buffer_is_full;
		}
	}

	printf("buffer: filled!\n");
	s->buffer_is_full = 1;
	return s->buffer_is_full;
}

void buffer_alloc(ngx_rtmp_session_t *s) {

    ngx_rtmp_live_app_conf_t *lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    BUFFER_SIZE = (int)lacf->kfbuflen;

	printf("buffer: alloc key frame buffer %d\n", BUFFER_SIZE);

	void **buffer = malloc(BUFFER_SIZE*sizeof(void *));
	for (int i=0; i<=BUFFER_SIZE; i++) {
		buffer[i] = NULL;
	}
	s->buffer = buffer;
	s->buffer_is_full = 0;
	s->buffer_i = 0;
	s->buffer_was_bursted = 0;
}

int buffer_find_next(ngx_rtmp_session_t *s) {
	int next = s->buffer_i+1;
	if (next >= BUFFER_SIZE) {
		return 0;
	} else {
		return next;
	}
}

int buffer_find_kf_next(ngx_rtmp_session_t *s) {

	int start = -1;
	int i;
	int end = s->buffer_i;
	struct bufitem *pkt;

	for (i=end; i>=0; i--) {

		pkt = s->buffer[i];
		if (pkt != NULL && pkt->kf == 1) {
			start = i;
			break;
		}
	}
	if (start == -1) {
		for (i=BUFFER_SIZE; i>end; i--) {

			pkt = s->buffer[i];
			if (pkt != NULL && pkt->kf == 1) {
				start = i;
				break;
			}
		}
	}

	if (start == -1) {
		printf("buffer: no key frame in buffer!\n");
		return 0;
	}

	if (start >= BUFFER_SIZE) {
		start = 0;
	}

	printf("buffer: new client, sending from %d\n", start);
	return start;
}


int buffer_add(ngx_rtmp_session_t *s, struct bufitem *i) {

	struct bufitem *prev;

	int next = buffer_find_next(s);
	//printf(" buffer: add %d\n", next);
	if (i->kf == 1) printf("buffer: keyframe!\n");

	prev = s->buffer[next];
	s->buffer[next] = i;
	s->buffer_i = next;

	// free
	if (prev != NULL && prev->pkt != NULL) {
		ngx_rtmp_free_shared_chain(
				ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module),
				prev->pkt
				);
	}

	return next;
}



//struct bufitem *
int buffer_get_cur(ngx_rtmp_session_t *publisher, ngx_rtmp_session_t *receiver) {
	int next;
	if (receiver->buffer_i == -1) {
		next = buffer_find_kf_next(publisher);
	} else {
		next = buffer_find_next(receiver);
	}
	//printf("%d\n", next);
	receiver->buffer_i = next;
	return receiver->buffer_i;
	//return publisher->buffer[next];
}


static void buffer_burst(ngx_rtmp_session_t *s, ngx_rtmp_live_ctx_t *pctx)
{
	pctx->session->buffer_was_bursted = 1;

	int start = buffer_get_cur(s, pctx->session);
	start--;
	int end = s->buffer_i;
	struct bufitem *bi;
	int tend = end;
	int otherhalf = -1;
	int i;

	printf("buffer: bursting %d-%d\n", start, end);

	if (tend < start) {
		tend = BUFFER_SIZE;
		otherhalf = end;
	}

	for (i = start; i<tend; i++) {
		bi = s->buffer[i];
		buffer_send(s, bi, pctx);
	}

	if (otherhalf != -1) {
		for (i = 0; i<otherhalf; i++) {
			bi = s->buffer[i];
			buffer_send(s, bi, pctx);
		}
	}

	pctx->session->buffer_i = end;
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

	delta = bi->delta;
	ch = bi->ch;
	lh = bi->lh;
	clh = bi->clh;

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

static ngx_int_t
buffer_ngx_rtmp_live_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
		ngx_chain_t *in)
{
	struct bufitem *bi;
	struct bufitem *i;
	int start;
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
	h->timestamp = h->timestamp + get_offset(ctx->stream->name, s->current_time);
	/**
	 * timestamp fix end
	 */

	s->current_time = h->timestamp;
	lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);

	csidx = !(lacf->interleave || h->type == NGX_RTMP_MSG_VIDEO);
	cs  = &ctx->cs[csidx];

	i = malloc(sizeof(struct bufitem));
	i->pkt = ngx_rtmp_append_shared_bufs(cscf, NULL, in);
	if (prio == NGX_RTMP_VIDEO_KEY_FRAME) {
		i->kf = 1;
	} else {
		i->kf = 0;
	}
	ngx_rtmp_header_t *mh = malloc(sizeof(ngx_rtmp_header_t));
	mh->csid = h->csid;
	mh->timestamp = h->timestamp;
	mh->mlen = h->mlen;
	mh->type = h->type;
	mh->msid = h->msid;
	i->h = mh;
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

	ngx_rtmp_live_chunk_stream_t   *mcs = malloc(sizeof(ngx_rtmp_live_chunk_stream_t));

	mcs->active = cs->active;
	mcs->timestamp = cs->timestamp;
	mcs->csid = cs->csid;
	mcs->dropped = cs->dropped;
	i->cs = mcs;
	i->delta = delta;
	i->ch = ch;
	i->lh = lh;
	i->clh = clh;

	buffer_add(s, i);

	if (!buffer_is_full(s)) {
		if (s->buffer_i == 1)
			printf("buffer: buffer not yet filled\n");
		return NGX_OK;
	} else {


		for (pctx = ctx->stream->ctx; pctx; pctx = pctx->next) {

			if (pctx == ctx || pctx->paused) {
				continue;
			}

			start = buffer_get_cur(s, pctx->session);
			bi = s->buffer[start];

            //disabled
			if (0 && pctx->session->buffer_was_bursted == 0) {
				buffer_burst(s, pctx);
				continue;
			}

			buffer_send(s, bi, pctx);
		}

	}

	return NGX_OK;
}




