// A minimal HTTP/1.0 client: plain sockets, one request per connection.
//
// Every request takes a full URL. The client knows a server only by the URL of
// its screen (see config.h), and everything else it talks to — the actions in
// the tree, the ping URL in the envelope — is resolved against that, so no
// server's path layout is baked in here.
//
// Only http:// is supported. There is no TLS: on a home network that is the
// usual trade, but it does mean the token crosses the wire in the clear.

#ifndef KOBOARD_HTTP_H
#define KOBOARD_HTTP_H

#include <stddef.h>

struct HttpResponse {
	int   status;
	char *body;     // malloc'd, caller frees
	size_t body_len;
};

// A URL split into what a request needs. `path` keeps the query string.
struct Url {
	char host[128];
	int  port;
	char path[512];
};

// Split an http:// URL. Returns -1 for anything else (https:// included, with
// a message on stderr saying so, since that is the likely surprise).
int url_parse(const char *url, struct Url *out);

// Resolve `ref` against `base`, the way a browser would for the cases a UI tree
// uses: an absolute http:// URL passes through, "/x" is relative to base's
// origin, and anything else is relative to base's directory. Base's query
// string never carries over. Returns -1 if the result does not fit.
int url_resolve(const char *base, const char *ref, char *out, size_t outsz);

// `url` with `query` appended, after '?' or '&' as the URL needs. Returns -1
// if the result does not fit.
int url_with_query(const char *url, const char *query, char *out, size_t outsz);

// `token` is sent as "Authorization: Bearer <token>", or not at all if empty.
int http_get(const char *url, const char *token, struct HttpResponse *resp);

int http_post(const char *url, const char *token, const char *post_data,
	      struct HttpResponse *resp);

void http_free_response(struct HttpResponse *resp);

#endif
