#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include "evhtp.h"
#include <event2/dns.h>


#define MAX_OUTPUT (512*1024)

void relay(struct bufferevent *b_in, struct evdns_base *evdns_base, const char *hostname, int port);

#ifdef USE_THREAD
static struct evdns_base  * evdnss[128] = {};
#else
static struct evdns_base* evdns = NULL;
#endif

static void 
print_backend_error(evhtp_request_t * req, evhtp_error_flags errtype, void * arg) {
	evhtp_request_t * frontend_req = (evhtp_request_t *)arg;
	printf("evhtp backend error\n");
}

static void 
print_frontend_error(evhtp_request_t * req, evhtp_error_flags errtype, void * arg) {
	evhtp_request_t * backend_req = (evhtp_request_t *)arg;
	printf("evhtp frontend error\n");

	evhtp_request_pause(backend_req);
	evhtp_connection_t *ev_conn = evhtp_request_get_connection(backend_req);
	evhtp_connection_free(ev_conn);
}

static evhtp_res resume_backend_request(evhtp_connection_t * conn, void * arg) {
	evhtp_request_t * backend_req = (evhtp_request_t *)arg;

	printf("resume backend request\n");
	evhtp_request_resume(backend_req); // bug, client can't evhtp_request_resume

	evhtp_unset_hook(&conn->hooks, evhtp_hook_on_write);
	
	return EVHTP_RES_OK;
}


static evhtp_res
print_data(evhtp_request_t * req, evbuf_t * buf, void * arg) {
	evhtp_request_t * frontend_req = (evhtp_request_t *)arg;
	size_t len = evbuffer_get_length(buf);

	printf("relay http body, got %zu bytes\n", len);
	//fwrite(evbuffer_pullup(buf, len), 1, len, stdout);

	evhtp_send_reply_chunk(frontend_req, buf);
	
	evbuffer_drain(buf, -1); // remove readed data

	if(evbuffer_get_length(bufferevent_get_output(evhtp_request_get_bev(frontend_req))) > MAX_OUTPUT) {
		printf("too many data, stop backend request\n");
		evhtp_request_pause(req);

		evhtp_set_hook(&evhtp_request_get_connection(frontend_req)->hooks, evhtp_hook_on_write, resume_backend_request, req);
	}

	return EVHTP_RES_OK;
}

static evhtp_res print_headers(evhtp_request_t * backend_req, evhtp_headers_t * hdr, void * arg) {
	evhtp_request_t * frontend_req = (evhtp_request_t *)arg;
	evhtp_header_t *header = NULL;

    printf("all headers ok\n");
    evhtp_kv_t * kv;

    //TAILQ_FOREACH(kv, hdr, next) {
    //    printf("%*s:%s\n", kv->klen, kv->key, kv->val);
    //}

    evhtp_headers_add_headers(frontend_req->headers_out, hdr);

	//// Content-Length will be auto set by libevhtp
    //if((header = evhtp_kvs_find_kv(frontend_req->headers_out, "Transfer-Encoding"))) {
	//    evhtp_header_rm_and_free(frontend_req->headers_out, header);
    //}
    if((header = evhtp_kvs_find_kv(frontend_req->headers_out, "Connection"))) {
	    evhtp_header_rm_and_free(frontend_req->headers_out, header);
    }


    printf("backend http response.\n");
    //evhtp_send_reply(frontend_req, EVHTP_RES_OK);
    evhtp_send_reply_chunk_start(frontend_req, evhtp_request_status(backend_req));
    evhtp_request_resume(frontend_req);

    return EVHTP_RES_OK;
}

int
make_request(evbase_t         * evbase,
	     struct evdns_base* evdns,
             evthr_t          * evthr,
             const char * const host,
             const short        port,
             const char * const path,
	     htp_method 	method,
             evhtp_headers_t  * headers,
             evbuf_t    * 		body,
             evhtp_callback_cb  cb,
             void             * arg) {
    evhtp_connection_t * conn;
    evhtp_request_t    * request;
	evhtp_header_t *header = NULL;
    evhtp_request_t * frontend_req = (evhtp_request_t *)arg;

    conn         = evhtp_connection_new_dns(evbase, evdns, host, port);
    conn->thread = evthr;
    request      = evhtp_request_new(cb, arg);


    evhtp_headers_add_headers(request->headers_out, headers);
    if((header = evhtp_kvs_find_kv(request->headers_out, "Connection"))) {
	    evhtp_header_rm_and_free(request->headers_out, header);
    }
    if((header = evhtp_kvs_find_kv(request->headers_out, "Proxy-Connection"))) {
	    evhtp_header_rm_and_free(request->headers_out, header);
    }
    //if((header = evhtp_kvs_find_kv(request->headers_out, "Accept-Encoding"))) {
	//    evhtp_header_rm_and_free(request->headers_out, header);
    //}
    evhtp_headers_add_header(request->headers_out,
                             evhtp_header_new("Connection", "close", 0, 0));


    evbuffer_prepend_buffer(request->buffer_out, body);

	// hook
    evhtp_set_hook(&request->hooks, evhtp_hook_on_error, print_backend_error, arg);
    evhtp_set_hook(&request->hooks, evhtp_hook_on_headers, print_headers, arg);
    evhtp_set_hook(&request->hooks, evhtp_hook_on_read, print_data, arg);

    evhtp_set_hook(&frontend_req->hooks, evhtp_hook_on_error, print_frontend_error, request);

    printf("Making backend request...\n");
    evhtp_make_request(conn, request, method, path);
    printf("async.\n");

    return 0;
}

static void
backend_cb(evhtp_request_t * backend_req, void * arg) {
	//evhtp_header_t *header = NULL;
    evhtp_request_t * frontend_req = (evhtp_request_t *)arg;

    printf("finish http response.\n");
    evhtp_send_reply_chunk_end(frontend_req);

    evhtp_unset_hook(&frontend_req->hooks, evhtp_hook_on_error);
}

static void
frontend_cb(evhtp_request_t * req, void * arg) {
#ifdef USE_THREAD
    int * aux;
    int   thr;
	struct evdns_base  * evdns;

    aux = (int *)evthr_get_aux(req->conn->thread);
    thr = *aux;

    printf("  Received frontend request on thread %d... ", thr);
	evdns = evdnss[thr];
    evbase_t    * evbase  = evthr_get_base(req->conn->thread);
#else
	evbase_t    * evbase  = req->conn->evbase;
#endif

    const char *host = req->uri->authority->hostname; 
    uint16_t port = req->uri->authority->port ?  req->uri->authority->port : 80;
    printf("Ok. %s:%u\n", host, port);

    /* Pause the frontend request while we run the backend requests. */
    evhtp_request_pause(req);

    if (htp_method_CONNECT == req->method) {
	    evhtp_headers_add_header(req->headers_out,
			    evhtp_header_new("Connection", "Keep-Alive", 0, 0));
	    evhtp_headers_add_header(req->headers_out,
			    evhtp_header_new("Content-Length", "0", 0, 0));
	    evhtp_send_reply(req, EVHTP_RES_OK);

	    printf("relay http socket.\n");
	    evbev_t * bev = evhtp_request_take_ownership(req);
	    relay(bev, evdns, host, port);
    } else {
		evbuf_t *uri = evbuffer_new();
		if (req->uri->query_raw) {
			evbuffer_add_printf(uri, "%s?%s", req->uri->path->full, req->uri->query_raw);
		} else {
			evbuffer_add_reference(uri, req->uri->path->full, strlen(req->uri->path->full), NULL, NULL);
		}

	    make_request(evbase,
			    evdns,
			    req->conn->thread,
			    //"127.0.0.1", 80,
			    host, port,
			    (char*)evbuffer_pullup(uri, -1),
			    req->method,
			    req->headers_in, req->buffer_in, 
				backend_cb, req);

		evbuffer_free(uri);
    }
}

/* Terminate gracefully on SIGTERM */
void
sigterm_cb(int fd, short event, void * arg) {
    evbase_t     * evbase = (evbase_t *)arg;
    struct timeval tv     = { .tv_usec = 100000, .tv_sec = 0 }; /* 100 ms */

    event_base_loopexit(evbase, &tv);
}

#ifdef USE_THREAD
void
init_thread_cb(evhtp_t * htp, evthr_t * thr, void * arg) {
    static int aux = 0;

    printf("Spinning up a thread: %d\n", ++aux);
    evthr_set_aux(thr, &aux);
    evbase_t     * evbase = evthr_get_base(thr);
    evdnss[aux] = evdns_base_new(evbase, 1);
}
#endif

int
main(int argc, char ** argv) {
    struct event *ev_sigterm;
    evbase_t    * evbase  = event_base_new();
    evhtp_t     * evhtp   = evhtp_new(evbase, NULL);

#ifdef USE_THREAD
    evhtp_set_gencb(evhtp, frontend_cb, NULL);

#if 0
#ifndef EVHTP_DISABLE_SSL
    evhtp_ssl_cfg_t scfg1 = { 0 };

    scfg1.pemfile  = "./server.pem";
    scfg1.privfile = "./server.pem";

    evhtp_ssl_init(evhtp, &scfg1);
#endif
#endif

    evhtp_use_threads(evhtp, init_thread_cb, 2, NULL);
#else
    evdns = evdns_base_new(evbase, 1);
    evhtp_set_gencb(evhtp, frontend_cb, NULL);
#endif

#ifndef WIN32
    ev_sigterm = evsignal_new(evbase, SIGTERM, sigterm_cb, evbase);
    evsignal_add(ev_sigterm, NULL);
#endif
    evhtp_bind_socket(evhtp, "0.0.0.0", 8081, 1024);
    event_base_loop(evbase, 0);

    printf("Clean exit\n");
    return 0;
}

