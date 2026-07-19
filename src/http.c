#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define HTTP_TIMEOUT_SEC 10
#define HTTP_BUFSIZ      4096

static int http_connect(const char *host, int port)
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons((unsigned short) port);

	// Try numeric IP first — avoids DNS hang when network is down
	if (inet_aton(host, &addr.sin_addr) == 0) {
		struct hostent *he = gethostbyname(host);
		if (!he) return -1;
		memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t) he->h_length);
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;

	struct timeval tv = { HTTP_TIMEOUT_SEC, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static char *read_all(int fd, size_t *out_len)
{
	size_t cap = 4096;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf) return NULL;

	while (1) {
		if (len + 1024 > cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) { free(buf); return NULL; }
			buf = nb;
		}
		ssize_t n = read(fd, buf + len, 1024);
		if (n <= 0) break;
		len += (size_t) n;
	}
	*out_len = len;
	return buf;
}

static int http_request(int fd, const char *method, const char *host,
			const char *path, const char *token,
			const char *post_data)
{
	char buf[2048];
	int n;

	if (post_data) {
		n = snprintf(buf, sizeof(buf),
			"%s %s HTTP/1.0\r\n"
			"Host: %s\r\n"
			"X-API-Token: %s\r\n"
			"Content-Type: application/x-www-form-urlencoded\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n"
			"\r\n"
			"%s",
			method, path, host, token, strlen(post_data),
			post_data);
	} else {
		n = snprintf(buf, sizeof(buf),
			"%s %s HTTP/1.0\r\n"
			"Host: %s\r\n"
			"X-API-Token: %s\r\n"
			"Connection: close\r\n"
			"\r\n",
			method, path, host, token);
	}

	if (n < 0 || (size_t) n >= sizeof(buf)) return -1;

	ssize_t written = write(fd, buf, (size_t) n);
	if (written != n) return -1;
	return 0;
}

static int parse_status(const char *response, size_t len)
{
	if (len < 12) return 0;
	int status;
	if (sscanf(response, "HTTP/1.%*d %d", &status) == 1)
		return status;
	return 0;
}

static char *skip_headers(char *response, size_t len)
{
	char *p = response;
	for (size_t i = 0; i + 3 < len; i++) {
		if (p[i] == '\r' && p[i+1] == '\n' &&
		    p[i+2] == '\r' && p[i+3] == '\n')
			return p + i + 4;
	}
	return NULL;
}

static int do_request(const char *server_url, const char *path,
		      const char *method, const char *token,
		      const char *post_data, struct HttpResponse *resp)
{
	memset(resp, 0, sizeof(*resp));

	char host[128];
	int  port = 80;
	const char *p = server_url;
	if (strncmp(p, "http://", 7) == 0) p += 7;
	char *colon = strchr(p, ':');
	char *slash = strchr(p, '/');
	if (colon) {
		size_t hlen = (size_t) (colon - p);
		if (hlen >= sizeof(host)) return -1;
		memcpy(host, p, hlen);
		host[hlen] = '\0';
		port = atoi(colon + 1);
	} else if (slash) {
		size_t hlen = (size_t) (slash - p);
		if (hlen >= sizeof(host)) return -1;
		memcpy(host, p, hlen);
		host[hlen] = '\0';
	} else {
		strncpy(host, p, sizeof(host) - 1);
		host[sizeof(host) - 1] = '\0';
	}

	int fd = http_connect(host, port);
	if (fd < 0) return -1;

	if (http_request(fd, method, host, path, token, post_data) < 0) {
		close(fd);
		return -1;
	}

	size_t raw_len;
	char *raw = read_all(fd, &raw_len);
	close(fd);
	if (!raw) return -1;

	resp->status = parse_status(raw, raw_len);
	char *body_start = skip_headers(raw, raw_len);
	if (body_start && body_start < raw + raw_len) {
		size_t body_len = (size_t) (raw + raw_len - body_start);
		resp->body     = malloc(body_len + 1);
		if (resp->body) {
			memcpy(resp->body, body_start, body_len);
			resp->body[body_len] = '\0';
			resp->body_len = body_len;
		}
	}
	free(raw);
	return 0;
}

int http_get(const char *server_url, const char *path,
	     const char *token, struct HttpResponse *resp)
{
	return do_request(server_url, path, "GET", token, NULL, resp);
}

int http_post(const char *server_url, const char *path,
	      const char *token, const char *post_data,
	      struct HttpResponse *resp)
{
	return do_request(server_url, path, "POST", token, post_data, resp);
}

void http_free_response(struct HttpResponse *resp)
{
	free(resp->body);
	resp->body = NULL;
}
