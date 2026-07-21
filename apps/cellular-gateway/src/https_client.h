#ifndef NODIFYR_HTTPS_CLIENT_H_
#define NODIFYR_HTTPS_CLIENT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct nodifyr_http_response {
	int status_code;
	int retry_after_s;
	char *body;
	size_t body_len;
};

/**
 * Open a persistent TLS session to host:443 (NULL = CONFIG_NODIFYR_CLOUD_HOST).
 * Use with nodifyr_https_request() then nodifyr_https_session_end().
 * Saves a second handshake when upload + config share one radio burst.
 */
int nodifyr_https_session_begin(const char *host);

/** Close the session opened by nodifyr_https_session_begin(). */
void nodifyr_https_session_end(void);

/**
 * Perform HTTPS GET or POST to host:443.
 * If a session is open to the same host, reuses that TLS connection.
 * @param host Hostname for DNS/SNI; NULL uses CONFIG_NODIFYR_CLOUD_HOST
 * @param method "GET" or "POST"
 * @param path URL path beginning with '/'
 * @param query Optional query string without leading '?', or NULL
 * @param body Optional request body, or NULL
 * @param extra_headers NULL-terminated list of "Header: value" strings, or NULL
 * @param resp Filled on success; body points into internal buffer valid until next call
 */
int nodifyr_https_request(const char *host,
			  const char *method,
			  const char *path,
			  const char *query,
			  const char *body,
			  const char *const *extra_headers,
			  struct nodifyr_http_response *resp);

#endif /* NODIFYR_HTTPS_CLIENT_H_ */
