#include "https_client.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/tls_credentials.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(nodifyr_https, CONFIG_NODIFYR_LOG_LEVEL);

#define RECV_BUF_SIZE	4096
#define BODY_BUF_SIZE	4096

static uint8_t recv_buf[RECV_BUF_SIZE];
static char body_buf[BODY_BUF_SIZE];
static size_t body_len;
static int http_status;
static int retry_after_s;

struct resp_ctx {
	bool done;
};

static int parse_retry_after(const char *headers, size_t headers_len)
{
	const char *p;
	const char *end;
	char key[] = "retry-after:";

	if (!headers || headers_len == 0) {
		return 0;
	}

	end = headers + headers_len;
	p = headers;
	while (p + sizeof(key) < end) {
		if (strncasecmp(p, key, sizeof(key) - 1) == 0) {
			p += sizeof(key) - 1;
			while (p < end && (*p == ' ' || *p == '\t')) {
				p++;
			}
			return atoi(p);
		}
		while (p < end && *p != '\n') {
			p++;
		}
		if (p < end) {
			p++;
		}
	}

	return 0;
}

static int response_cb(struct http_response *rsp, enum http_final_call final_data,
		       void *user_data)
{
	struct resp_ctx *ctx = user_data;
	size_t copy;

	if (rsp->http_status_code > 0) {
		http_status = rsp->http_status_code;
	}

	/* NCS/Zephyr http_response no longer exposes header_start; headers
	 * occupy recv_buf until body_frag_start when both are present.
	 */
	if (retry_after_s == 0 && rsp->recv_buf && rsp->data_len > 0) {
		size_t header_len = rsp->data_len;

		if (rsp->body_frag_start) {
			if (rsp->body_frag_start > rsp->recv_buf) {
				header_len = (size_t)(rsp->body_frag_start -
						      rsp->recv_buf);
			} else {
				header_len = 0;
			}
		}

		if (header_len > 0) {
			retry_after_s = parse_retry_after(
				(const char *)rsp->recv_buf, header_len);
		}
	}

	if (rsp->body_frag_start && rsp->body_frag_len > 0) {
		copy = rsp->body_frag_len;
		if (body_len + copy >= sizeof(body_buf)) {
			copy = sizeof(body_buf) - 1 - body_len;
		}
		if (copy > 0) {
			memcpy(&body_buf[body_len], rsp->body_frag_start, copy);
			body_len += copy;
			body_buf[body_len] = '\0';
		}
	}

	if (final_data == HTTP_DATA_FINAL) {
		ctx->done = true;
	}

	return 0;
}

static int setup_tls_socket(int sock, const char *hostname)
{
	int err;
	int verify = TLS_PEER_VERIFY_REQUIRED;
	sec_tag_t sec_tag_list[] = { CONFIG_NODIFYR_TLS_SEC_TAG };

	err = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify,
			       sizeof(verify));
	if (err) {
		LOG_ERR("TLS_PEER_VERIFY failed: %d", errno);
		return -errno;
	}

	err = zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, sec_tag_list,
			       sizeof(sec_tag_list));
	if (err) {
		LOG_ERR("TLS_SEC_TAG_LIST failed: %d", errno);
		return -errno;
	}

	err = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, hostname,
			       strlen(hostname));
	if (err) {
		LOG_ERR("TLS_HOSTNAME failed: %d", errno);
		return -errno;
	}

	return 0;
}

static int connect_host(const char *hostname, uint16_t port)
{
	int err;
	int sock = -1;
	struct zsock_addrinfo hints = { 0 };
	struct zsock_addrinfo *res = NULL;
	char port_str[8];

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	snprintk(port_str, sizeof(port_str), "%u", port);

	err = zsock_getaddrinfo(hostname, port_str, &hints, &res);
	if (err) {
		LOG_ERR("DNS %s failed: %d", hostname, err);
		return -EIO;
	}

	sock = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (sock < 0) {
		LOG_ERR("socket failed: %d", errno);
		err = -errno;
		goto out;
	}

	err = setup_tls_socket(sock, hostname);
	if (err) {
		zsock_close(sock);
		sock = err;
		goto out;
	}

	err = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	if (err) {
		LOG_ERR("connect %s:%u failed: %d", hostname, port, errno);
		zsock_close(sock);
		sock = -errno;
		goto out;
	}

	sock = sock;

out:
	zsock_freeaddrinfo(res);
	return sock;
}

static enum http_method method_from_str(const char *method)
{
	if (method && strcmp(method, "POST") == 0) {
		return HTTP_POST;
	}
	return HTTP_GET;
}

int nodifyr_https_request(const char *host,
			  const char *method,
			  const char *path,
			  const char *query,
			  const char *body,
			  const char *const *extra_headers,
			  struct nodifyr_http_response *resp)
{
	int sock;
	int err;
	struct http_request req = { 0 };
	struct resp_ctx ctx = { 0 };
	char url[256];
	static const char *default_headers[] = {
		"Content-Type: application/json\r\n",
		NULL,
	};
	const char *const *headers = extra_headers;

	if (!method || !path || !resp) {
		return -EINVAL;
	}

	if (!host || host[0] == '\0') {
		host = CONFIG_NODIFYR_CLOUD_HOST;
	}

	if (!headers) {
		if (body) {
			headers = default_headers;
		}
	}

	if (query && query[0] != '\0') {
		err = snprintk(url, sizeof(url), "%s?%s", path, query);
	} else {
		err = snprintk(url, sizeof(url), "%s", path);
	}
	if (err < 0 || err >= (int)sizeof(url)) {
		return -ENOMEM;
	}

	body_len = 0;
	body_buf[0] = '\0';
	http_status = 0;
	retry_after_s = 0;
	memset(resp, 0, sizeof(*resp));

	LOG_INF("%s https://%s%s", method, host, url);

	sock = connect_host(host, 443);
	if (sock < 0) {
		return sock;
	}

	req.method = method_from_str(method);
	req.url = url;
	req.host = host;
	req.protocol = "HTTP/1.1";
	req.response = response_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = sizeof(recv_buf);
	req.header_fields = headers ? (const char **)headers : NULL;

	if (body) {
		req.payload = body;
		req.payload_len = strlen(body);
	}

	err = http_client_req(sock, &req, CONFIG_NODIFYR_HTTP_TIMEOUT_MS, &ctx);
	zsock_close(sock);

	if (err < 0) {
		LOG_ERR("http_client_req: %d", err);
		return err;
	}

	resp->status_code = http_status;
	resp->retry_after_s = retry_after_s;
	resp->body = body_buf;
	resp->body_len = body_len;

	LOG_INF("HTTP %d (%u body bytes)", http_status, (unsigned)body_len);
	return 0;
}
