#ifndef NGX_RTMP_BUFSHARED_H
#define NGX_RTMP_BUFSHARED_H

#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_codec_module.h"

typedef struct bufstr {
  char                            *name;
  void                            **buffer;
  int                             *buffer_i;
  int                             *buffer_was_bursted;
  int                             *buffer_is_full;
  int                             *buffer_is_allocated;
  ngx_rtmp_session_t              *s;
  struct bufreceiver              *r;
  struct bufstr                   *next;
} bufstr;

struct bufstr *bufstr_get (char *name);

void buffer_publisher_free (char *name, ngx_rtmp_session_t *r);
void buffer_free(ngx_rtmp_session_t *s);
ngx_int_t buffer_ngx_rtmp_live_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h, ngx_chain_t *in);
void bufstr_upsert (char *name, ngx_rtmp_session_t *s);
void buffer_init();
void buffer_reset_buffer_i (ngx_rtmp_session_t *s);
char *buffer_get_pointer_as_string(void *p);
char *get_name_from_v(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v);
void buffer_alloc(ngx_rtmp_session_t *s);

extern bufstr *root_bufstr;

#endif