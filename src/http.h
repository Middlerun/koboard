#ifndef HOMESRV_HTTP_H
#define HOMESRV_HTTP_H

#include <stddef.h>

struct HttpResponse {
	int   status;
	char *body;     // malloc'd, caller frees
	size_t body_len;
};

int http_get(const char *server_url, const char *path,
	     const char *token, struct HttpResponse *resp);

int http_post(const char *server_url, const char *path,
	      const char *token, const char *post_data,
	      struct HttpResponse *resp);

void http_free_response(struct HttpResponse *resp);

#endif
