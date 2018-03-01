#include <evhttp.h>
#include "ngx_rtmp_live_module.h"
//#include "ngx_rtmp_bufshared.h"
//
// apt install libevent-dev
//gcc main.c -levent -o main.o


void do_disconnect(const char *app, const char *stream) {

	char *name = malloc(sizeof(char)*(strlen(app)+strlen(stream)+1));
	strcpy(name, app);
	strcat(name, "/");
	strcat(name, stream);

	bufstr *bp = bufstr_get(name);
	if (bp!=NULL) {
		ngx_rtmp_session_t *s = bp->s;

		ngx_chain_t * control = ngx_rtmp_create_stream_eof(s, NGX_RTMP_MSID);

		if (control && ngx_rtmp_send_message(bp->s, control, 0) != NGX_OK) {
			ngx_rtmp_finalize_session(s);
			return;
		}
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

	printf("disconnecting listeners %s %s\n", app, stream);
	do_disconnect(app, stream);

	// Create an answer buffer where the data to send back to the browser will be appened
	buffer = evbuffer_new ();
	evbuffer_add (buffer, "OK", 2);


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

void *do_start_server () {


	struct event_base *ebase;
	struct evhttp *server;

	// Create a new event handler
	ebase = event_base_new ();;

	// Create a http server using that handler
	server = evhttp_new (ebase);

	// Limit serving GET requests
	evhttp_set_allowed_methods (server, EVHTTP_REQ_GET);

	// Set a test callback, /testing
	evhttp_set_cb (server, "/disconnect", handle_disconnect, 0);

	// Set the callback for anything not recognized
	evhttp_set_gencb (server, notfound, 0);

	// Listen locally on port 32001
	if (evhttp_bind_socket (server, "127.0.0.1", 32001) != 0)
		printf("Could not bind to 127.0.0.1:32001");

	// Start processing queries
	event_base_dispatch(ebase);

	// Free up stuff
	evhttp_free (server);

	event_base_free (ebase);

	return NULL;

}

void start_server() {

	printf("api start at 32001\n");

	pthread_t tid;
	pthread_create(&tid, NULL, &do_start_server, NULL);

	printf("api init done\n");
}

