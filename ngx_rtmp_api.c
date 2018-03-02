#include <evhttp.h>
#include <stdlib.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_process.h>
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_bufshared.h"

// apt install libevent-dev
//gcc main.c -levent -o main.o

int api_port = 32000;

void do_disconnect(const char *app, const char *stream) {

	char *name = malloc(sizeof(char)*(strlen(app)+strlen(stream)+1));
	strcpy(name, app);
	strcat(name, "/");
	strcat(name, stream);

	printf("%s!\n", name);

	bufstr *bp = bufstr_get(name);
	if (bp != NULL) {
		//printf("process %d: disconnecting viewers from '%s/%s'\n", (int) ngx_process_slot, app, stream);
		//ngx_rtmp_session_t *s = bp->s;

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
	// Parse the query for later lookups
	evhttp_parse_query (evhttp_request_get_uri (request), &headers);

	app = evhttp_find_header (&headers, "app");
	stream = evhttp_find_header (&headers, "stream");

	do_disconnect(app, stream);

	// Create an answer buffer where the data to send back to the browser will be appened
	buffer = evbuffer_new ();
	evbuffer_add (buffer, "ok", 2);

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


void disconnect_request_done(struct evhttp_request *req, void *arg){
    //char buf[1024];
    //int s = evbuffer_remove(req->input_buffer, &buf, sizeof(buf) - 1);
    //buf[s] = '\0';
    //printf("%s\n", buf);
    // terminate event_base_dispatch()
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
	int n = (int) ngx_last_process;
	for (int i=0; i<n; i++) {
        char *url = malloc(sizeof(char)*(strlen(app)+strlen(stream)+100));
		int port = api_port+i+1;

		strcpy(url, "/do_disconnect?app=");
		strcat(url, app);
		strcat(url, "&stream=");
		strcat(url, stream);

		struct event_base *base;
        struct evhttp_connection *conn;
        struct evhttp_request *req;

        base = event_base_new();
        conn = evhttp_connection_base_new(base, NULL, "127.0.0.1", port);
        req = evhttp_request_new(disconnect_request_done, base);

        evhttp_add_header(req->output_headers, "Host", "localhost");
        //evhttp_add_header(req->output_headers, "Connection", "close");

        evhttp_make_request(conn, req, EVHTTP_REQ_GET, url);
        evhttp_connection_set_timeout(req->evcon, 600);
        event_base_dispatch(base);
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

	// Create a new event handler
	ebase = event_base_new ();;

	// Create a http server using that handler
	server = evhttp_new (ebase);

	// Limit serving GET requests
	evhttp_set_allowed_methods (server, EVHTTP_REQ_GET);

	// Set a test callback, /testing
	evhttp_set_cb (server, "/disconnect", handle_disconnect_master, 0);

	// Set a test callback, /testing
	evhttp_set_cb (server, "/do_disconnect", handle_disconnect, 0);

	// Set the callback for anything not recognized
	evhttp_set_gencb (server, notfound, 0);

	// Listen locally on port 32001

	char *ip = malloc(sizeof(char)*12);
	int *port_i = (int *) port;

	if (api_port == *port_i) {
		ip = "0.0.0.0";
	} else {
		ip = "127.0.0.1";
	}

	if (evhttp_bind_socket (server, ip, (int) *port_i) != 0)
		printf("Could not bind to %s:%d", ip, (int) *port_i);

	// Start processing queries
	event_base_dispatch(ebase);

	// Free up stuff
	evhttp_free (server);

	event_base_free (ebase);

	return NULL;

}

void start_server(int port) {

	pthread_t tid;
	int *port_i = malloc(sizeof(int));
	*port_i = port;
	pthread_create(&tid, NULL, &do_start_server, port_i);
}

