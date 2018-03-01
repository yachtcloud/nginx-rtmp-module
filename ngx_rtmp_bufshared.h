#ifndef NGX_RTMP_BUFSHARED_H
#define NGX_RTMP_BUFSHARED_H

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
extern struct bufstr *bufstr_get (char *name);
extern bufstr *root_bufstr;

#endif