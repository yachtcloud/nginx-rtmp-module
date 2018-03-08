#define _GNU_SOURCE
#include <pthread.h>
#include <evhttp.h>
#include <stdlib.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_process.h>
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_bufshared.h"

// to use this module install libevent-dev:
//   apt install libevent-dev
//   gcc main.c -levent -o main.o

int api_port = 32000;
int is_port_open(int portno);

void do_stop(ngx_rtmp_session_t *s) {

	ngx_rtmp_core_srv_conf_t   *cscf;
    ngx_chain_t                *control;
    ngx_chain_t                *status[3];
    size_t                      n;

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    control = ngx_rtmp_create_stream_eof(s, NGX_RTMP_MSID);

    status[0] = ngx_rtmp_create_status(s,
                "NetStream.Play.UnpublishNotify",
                "status", "Stop publishing");

    ngx_rtmp_live_set_status(s, control, status, 1, 0);

    if (control) {
        ngx_rtmp_free_shared_chain(cscf, control);
    }

    for (n = 0; n < 1; ++n) {
        ngx_rtmp_free_shared_chain(cscf, status[n]);
    }
}

void do_disconnect(const char *app, const char *stream) {

	char *name = malloc(sizeof(char)*(strlen(app)+strlen(stream)+1));
	strcpy(name, app);
	strcat(name, "/");
	strcat(name, stream);

	printf("%s!\n", name);

	bufstr *bp = bufstr_get(name);

    free(name);
	if (bp != NULL) {
		//printf("process %d: disconnecting viewers from '%s/%s'\n", (int) ngx_process_slot, app, stream);
        if (bp->s != NULL) {
            ngx_rtmp_session_t *s = bp->s;
            do_stop(s);
        }

		printf("process %d: ok\n", (int) ngx_process_slot);
	} else {
		printf("process %d: not found\n", (int) ngx_process_slot);
	}
}


void handle_disconnect (struct evhttp_request *request, void *privParams) {
	struct evbuffer *buffer;
	struct evkeyvalq headers;
	const char *app;
	const char *stream;

	evhttp_parse_query (evhttp_request_get_uri (request), &headers);
	app = evhttp_find_header (&headers, "app");
	stream = evhttp_find_header (&headers, "stream");

    if (app != NULL && stream != NULL) {
    	do_disconnect(app, stream);
    }

	buffer = evbuffer_new ();
	evbuffer_add (buffer, "ok", 2);

	evhttp_add_header (evhttp_request_get_output_headers (request),
			"Content-Type", "text/plain");
	// reply
	evhttp_send_reply(request, HTTP_OK, "OK", buffer);
	evhttp_clear_headers (&headers);
	evbuffer_free (buffer);

	return;
}

void disconnect_request_done(struct evhttp_request *req, void *arg){
    event_base_loopbreak((struct event_base *)arg);
}

void handle_disconnect_master (struct evhttp_request *request, void *privParams) {
	struct evbuffer *buffer;
	struct evkeyvalq headers;
	const char *app;
	const char *stream;
	// Parse the query for later lookups
	evhttp_parse_query (evhttp_request_get_uri (request), &headers);

	app = evhttp_find_header (&headers, "app");
	stream = evhttp_find_header (&headers, "stream");

	//send request to all apis for each worker process
    if (app != NULL && stream != NULL) {
        int n = n_workers;//(int) ngx_last_process;
        char *url = malloc(sizeof(char)*(strlen(app)+strlen(stream)+100));

        int port;

        printf("n %d\n", n);

        for (int i=0; i<n; i++) {

            strcpy(url, "/do_disconnect?app=");
            strcat(url, app);
            strcat(url, "&stream=");
            strcat(url, stream);

            port = api_port+i+1;

            if (is_port_open(port) == 1) {

                printf("sending %d %s\n", port, url);

                struct event_base *base;
                struct evhttp_connection *conn;
                struct evhttp_request *req;

                base = event_base_new();
                conn = evhttp_connection_base_new(base, NULL, "127.0.0.1", port);
                req = evhttp_request_new(disconnect_request_done, base);

                evhttp_add_header(req->output_headers, "Host", "localhost");
                //evhttp_add_header(req->output_headers, "Connection", "close");

                evhttp_make_request(conn, req, EVHTTP_REQ_GET, url);
                evhttp_connection_set_timeout(req->evcon, 1000);
                event_base_dispatch(base);
                
                event_base_free (base);
            }
        }

        free(url);
    }

	// Create an answer buffer where the data to send back to the browser will be appened
	buffer = evbuffer_new ();
	evbuffer_add (buffer, "sent", 4);

	// Add a HTTP header, an application/json for the content type here
	evhttp_add_header (evhttp_request_get_output_headers (request),
			"Content-Type", "text/plain");

	// Tell we're done and data should be sent back
	evhttp_send_reply(request, HTTP_OK, "OK", buffer);

	// Free up stuff
	evhttp_clear_headers (&headers);

	evbuffer_free (buffer);

	return;
}

void notfound (struct evhttp_request *request, void *params) {
	evhttp_send_error(request, HTTP_NOTFOUND, "Not Found");
}

void *do_start_server (void *port) {

	struct event_base *ebase;
	struct evhttp *server;

	ebase = event_base_new ();;
	server = evhttp_new (ebase);
	evhttp_set_allowed_methods (server, EVHTTP_REQ_GET);

	evhttp_set_cb (server, "/disconnect", handle_disconnect_master, 0);
	evhttp_set_cb (server, "/do_disconnect", handle_disconnect, 0);

	evhttp_set_gencb (server, notfound, 0);

	int *port_i = (int *) port;

    printf("do_start_server at %d ip %s\n", *port_i,  api_port == *port_i ? "0.0.0.0" : "127.0.0.1");

	if (evhttp_bind_socket (server, api_port == *port_i ? "0.0.0.0" : "127.0.0.1", (int) *port_i) != 0)
		printf("Could not bind to port %d", (int) *port_i);

	event_base_dispatch(ebase);

	evhttp_free (server);
	event_base_free (ebase);
	return NULL;
}

int is_port_open(int portno) {

    char *hostname = "localhost";

    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return 0;
    }

    server = gethostbyname(hostname);

    if (server == NULL) {
        close(sockfd);
        return 0;
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
         (char *)&serv_addr.sin_addr.s_addr,
         server->h_length);

    serv_addr.sin_port = htons(portno);
    if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
        close(sockfd);
        return 0;
    } else {
        close(sockfd);
        return 1;
    }


}

void start_server(int port) {
    if (is_port_open(port) == 0) {
        printf("starting server at port %d\n", port);
        int *port_i = malloc(sizeof(int));
	    *port_i = port;
        pthread_t tid;
        pthread_create(&tid, NULL, &do_start_server, port_i);
    } else {
        printf("port %d is already occupied\n", port);
    }
}

