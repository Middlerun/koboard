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

static int http_request(int fd, const char *method, const char *host, int port,
			const char *path, const char *token,
			const char *post_data)
{
	char buf[4096];
	char hostline[160];
	char auth[128] = "";

	// The port belongs in Host whenever it is not the default, or a virtual
	// host on a non-standard port will not recognise the request.
	if (port == 80) snprintf(hostline, sizeof(hostline), "%s", host);
	else            snprintf(hostline, sizeof(hostline), "%s:%d", host, port);

	if (token && *token)
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", token);

	int n;
	if (post_data) {
		n = snprintf(buf, sizeof(buf),
			"%s %s HTTP/1.0\r\n"
			"Host: %s\r\n"
			"%s"
			"Content-Type: application/x-www-form-urlencoded\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n"
			"\r\n"
			"%s",
			method, path, hostline, auth, strlen(post_data),
			post_data);
	} else {
		n = snprintf(buf, sizeof(buf),
			"%s %s HTTP/1.0\r\n"
			"Host: %s\r\n"
			"%s"
			"Connection: close\r\n"
			"\r\n",
			method, path, hostline, auth);
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

// ---- URLs ----------------------------------------------------------------

int url_parse(const char *url, struct Url *out)
{
	memset(out, 0, sizeof(*out));
	if (!url) return -1;

	if (strncmp(url, "https://", 8) == 0) {
		fprintf(stderr, "koboard: %s: https is not supported, "
			"only http://\n", url);
		return -1;
	}
	if (strncmp(url, "http://", 7) != 0) return -1;

	const char *host = url + 7;
	size_t authority = strcspn(host, "/?#");
	const char *rest = host + authority;

	// host[:port], looking for the colon only inside the authority, so a
	// colon later in the path cannot be mistaken for a port.
	const char *colon = memchr(host, ':', authority);
	size_t hlen = colon ? (size_t) (colon - host) : authority;
	if (hlen == 0 || hlen >= sizeof(out->host)) return -1;
	memcpy(out->host, host, hlen);
	out->host[hlen] = '\0';

	out->port = 80;
	if (colon) {
		out->port = atoi(colon + 1);
		if (out->port <= 0 || out->port > 65535) return -1;
	}

	// The fragment is the browser's business, never the server's.
	size_t plen = strcspn(rest, "#");
	if (plen == 0) {
		snprintf(out->path, sizeof(out->path), "/");
	} else {
		if (plen + (*rest == '/' ? 0 : 1) >= sizeof(out->path)) return -1;
		snprintf(out->path, sizeof(out->path), "%s%.*s",
			 *rest == '/' ? "" : "/", (int) plen, rest);
	}
	return 0;
}

int url_resolve(const char *base, const char *ref, char *out, size_t outsz)
{
	if (!ref || !*ref) ref = "";

	if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
		int n = snprintf(out, outsz, "%s", ref);
		return (n < 0 || (size_t) n >= outsz) ? -1 : 0;
	}

	// The origin: everything up to the first '/' after the scheme.
	const char *after = strstr(base, "://");
	after = after ? after + 3 : base;
	size_t origin = (size_t) (after - base) + strcspn(after, "/?#");

	if (ref[0] == '/') {
		int n = snprintf(out, outsz, "%.*s%s", (int) origin, base, ref);
		return (n < 0 || (size_t) n >= outsz) ? -1 : 0;
	}

	// Relative: base's path up to and including its last '/', without the
	// query — "http://h/api/ui.php?w=1" + "x.php" is "http://h/api/x.php".
	size_t pathlen = strcspn(base + origin, "?#");
	size_t dir = origin;
	for (size_t i = origin; i < origin + pathlen; i++)
		if (base[i] == '/') dir = i + 1;

	int n;
	if (dir == origin)   // no path at all: "http://h" + "x" is "http://h/x"
		n = snprintf(out, outsz, "%.*s/%s", (int) origin, base, ref);
	else
		n = snprintf(out, outsz, "%.*s%s", (int) dir, base, ref);
	return (n < 0 || (size_t) n >= outsz) ? -1 : 0;
}

int url_with_query(const char *url, const char *query, char *out, size_t outsz)
{
	size_t len = strcspn(url, "#");
	const char *sep = memchr(url, '?', len) ? "&" : "?";
	int n = snprintf(out, outsz, "%.*s%s%s", (int) len, url, sep, query);
	return (n < 0 || (size_t) n >= outsz) ? -1 : 0;
}

// ---- requests ------------------------------------------------------------

static int do_request(const char *url, const char *method, const char *token,
		      const char *post_data, struct HttpResponse *resp)
{
	memset(resp, 0, sizeof(*resp));

	struct Url u;
	if (url_parse(url, &u) < 0) return -1;

	int fd = http_connect(u.host, u.port);
	if (fd < 0) return -1;

	if (http_request(fd, method, u.host, u.port, u.path, token, post_data) < 0) {
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

int http_get(const char *url, const char *token, struct HttpResponse *resp)
{
	return do_request(url, "GET", token, NULL, resp);
}

int http_post(const char *url, const char *token, const char *post_data,
	      struct HttpResponse *resp)
{
	return do_request(url, "POST", token, post_data, resp);
}

void http_free_response(struct HttpResponse *resp)
{
	free(resp->body);
	resp->body = NULL;
}
