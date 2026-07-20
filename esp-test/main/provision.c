#include "provision.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dns_server.h"
#include "nvs_config.h"
#include "oled.h"
#include "sdkconfig.h"
#include "wifi.h"

#include "driver/gpio.h"

static const char *TAG = "provision";

#define PROVISION_TIMEOUT_SEC (15 * 60)
#define FORM_BUF_SIZE         512
#define HTTP_RESP_BUF_SIZE    768
#define PROVISION_CARD_BUF    1024

static volatile bool s_serial_provision;
static httpd_handle_t s_server;
static dns_server_handle_t s_dns;
static char s_captive_portal_uri[32];

/* Set by the /configure handler once the user submits. The portal loop in
 * provision_run() picks this up, tears the SoftAP down and finishes setup on
 * the STA link, so the form's HTTP response is delivered before the AP drops. */
static volatile bool s_config_ready;
static char s_pending_ssid[NODIFYR_WIFI_SSID_MAX];
static char s_pending_claim[16];

/* Card body scratch (error/success pages). Shell HTML is streamed in chunks
 * so the full styled page never has to fit in one buffer. */
static char s_card_buf[PROVISION_CARD_BUF];

static const char PROVISION_HTML_STYLE[] =
    "<style>"
    ":root{--iron:#1D232B;--orange:#D45E1B;--paper:#F2F0E8;--slate:#809AA6;--red:#C0392B;--card:#252D37}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{min-height:100vh;font-family:system-ui,-apple-system,BlinkMacSystemFont,sans-serif;"
    "background:var(--iron);color:var(--paper);display:flex;align-items:center;justify-content:center;"
    "padding:24px 16px;background-image:radial-gradient(rgba(128,154,166,.22) 1px,transparent 1px);"
    "background-size:18px 18px}"
    ".w{width:100%;max-width:420px}"
    ".b{display:flex;align-items:center;justify-content:center;gap:8px;margin-bottom:22px;"
    "color:var(--orange);font-weight:700;font-size:12px;letter-spacing:.14em}"
    ".i{width:16px;height:16px}"
    ".c{background:var(--card);border:1px solid rgba(128,154,166,.22);border-radius:14px;"
    "padding:28px 24px;box-shadow:0 16px 48px rgba(0,0,0,.35)}"
    "h1{font-size:21px;font-weight:700;letter-spacing:.07em;text-transform:uppercase;margin-bottom:10px}"
    ".s{color:var(--slate);font-size:14px;line-height:1.55;margin-bottom:22px}"
    "label{display:block;color:var(--slate);font-size:10px;font-weight:600;letter-spacing:.09em;"
    "text-transform:uppercase;margin-bottom:6px}"
    "input,select{width:100%;padding:12px 14px;margin-bottom:16px;background:var(--iron);"
    "border:1px solid rgba(128,154,166,.32);border-radius:8px;color:var(--paper);font-size:16px;"
    "outline:none}"
    "input{-webkit-appearance:none}"
    "select{cursor:pointer}"
    "select option{background:var(--iron);color:var(--paper)}"
    "input:focus,select:focus{border-color:var(--orange);box-shadow:0 0 0 2px rgba(212,94,27,.22)}"
    "input::placeholder{color:rgba(128,154,166,.65)}"
    "button,.btn{display:block;width:100%;padding:14px;border:0;border-radius:8px;"
    "background:var(--orange);color:var(--paper);font-size:12px;font-weight:700;"
    "letter-spacing:.1em;text-transform:uppercase;text-align:center;text-decoration:none;"
    "cursor:pointer;margin-top:6px}"
    "button:active{opacity:.92}"
    ".err h1{color:var(--red)}"
    ".ok h1{color:var(--orange)}"
    "a.link{color:var(--orange);font-size:14px;text-decoration:none;display:inline-block;margin-top:18px}"
    "a.link:hover{text-decoration:underline}"
    "</style>";

static const char PROVISION_HTML_BRAND[] =
    "<div class=b><svg class=i viewBox=\"0 0 24 24\" fill=none stroke=currentColor stroke-width=2>"
    "<circle cx=12 cy=12 r=4/><path d=\"M12 2v4M12 18v4M2 12h4M18 12h4\"/></svg>NODIFYR</div>";

static const char *CAPTIVE_PORTAL_BODY = "Redirect to the captive portal";

static void url_decode(char *s)
{
    char *src = s;
    char *dst = s;

    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = body;

    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t tok_len = amp ? (size_t)(amp - p) : strlen(p);
        if (tok_len > key_len + 1 && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            size_t val_len = tok_len - key_len - 1;
            if (val_len >= out_len) {
                val_len = out_len - 1;
            }
            memcpy(out, p + key_len + 1, val_len);
            out[val_len] = '\0';
            url_decode(out);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }

    out[0] = '\0';
    return false;
}

static void normalize_claim_code(char *code)
{
    char compact[16] = {0};
    size_t j = 0;

    for (size_t i = 0; code[i] != '\0' && j < sizeof(compact) - 1; i++) {
        if (code[i] == '-' || code[i] == ' ') {
            continue;
        }
        compact[j++] = (char)toupper((unsigned char)code[i]);
    }

    if (j == 8) {
        snprintf(code, 16, "%.4s-%.4s", compact, compact + 4);
    } else {
        strncpy(code, compact, 15);
        code[15] = '\0';
    }
}

static void provision_oled_show(const char *line1, const char *line2, const char *line3)
{
    if (!oled_is_ready()) {
        return;
    }

    u8g2_t *display = oled_get();
    oled_lock();
    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_helvB08_tr);
    u8g2_DrawStr(display, 0, 10, "NODIFYR SETUP");
    u8g2_SetFont(display, u8g2_font_helvR08_tr);
    if (line1) {
        u8g2_DrawStr(display, 0, 24, line1);
    }
    if (line2) {
        u8g2_DrawStr(display, 0, 36, line2);
    }
    if (line3) {
        u8g2_DrawStr(display, 0, 48, line3);
    }
    u8g2_SendBuffer(display);
    oled_unlock();
}

static esp_err_t send_chunk(httpd_req_t *req, const char *chunk)
{
    return httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);
}

static void html_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_len; i++) {
        const char *rep = NULL;
        switch (in[i]) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default:   break;
        }
        if (rep != NULL) {
            size_t rl = strlen(rep);
            if (o + rl >= out_len) {
                break;
            }
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

static esp_err_t send_shell_open(httpd_req_t *req, int status)
{
    httpd_resp_set_status(req, status == 200 ? "200 OK" : "500 Internal Server Error");
    httpd_resp_set_type(req, "text/html");

    const char *chunks[] = {
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Nodifyr Gateway</title>",
        PROVISION_HTML_STYLE,
        "</head><body><div class=w>",
        PROVISION_HTML_BRAND,
    };

    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        if (send_chunk(req, chunks[i]) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t send_shell_close(httpd_req_t *req)
{
    if (send_chunk(req, "</div></body></html>") != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t send_provision_shell(httpd_req_t *req, int status, const char *body)
{
    if (send_shell_open(req, status) != ESP_OK) {
        return ESP_FAIL;
    }
    if (body != NULL && send_chunk(req, body) != ESP_OK) {
        return ESP_FAIL;
    }
    return send_shell_close(req);
}

/* Streams the setup form. When the boot-time scan found networks they are
 * offered as an RSSI-sorted dropdown, with a free-text field for hidden/other
 * networks that overrides the selection. */
static esp_err_t send_provision_form(httpd_req_t *req)
{
    if (send_chunk(req,
            "<div class=c><h1>Gateway Setup</h1>"
            "<p class=s>Connect this gateway to your WiFi using the setup code "
            "from the Nodifyr dashboard.</p>"
            "<form method=POST action=/configure>") != ESP_OK) {
        return ESP_FAIL;
    }

    size_t n = wifi_scan_count();
    if (n > 0) {
        if (send_chunk(req, "<label>WiFi Network</label><select name=ssid>") != ESP_OK) {
            return ESP_FAIL;
        }
        for (size_t i = 0; i < n; i++) {
            char ssid[NODIFYR_WIFI_SSID_MAX];
            bool secure = false;
            if (!wifi_scan_get(i, ssid, sizeof(ssid), NULL, &secure)) {
                continue;
            }
            char esc[NODIFYR_WIFI_SSID_MAX * 6];
            html_escape(ssid, esc, sizeof(esc));
            char opt[sizeof(esc) * 2 + 64];
            snprintf(opt, sizeof(opt), "<option value=\"%s\">%s%s</option>",
                     esc, esc, secure ? "" : " (open)");
            if (send_chunk(req, opt) != ESP_OK) {
                return ESP_FAIL;
            }
        }
        if (send_chunk(req,
                "</select>"
                "<label>Other Network</label>"
                "<input name=ssid_other autocomplete=off placeholder='Hidden or not listed'>") != ESP_OK) {
            return ESP_FAIL;
        }
    } else if (send_chunk(req,
                   "<label>WiFi SSID</label>"
                   "<input name=ssid required autocomplete=off>") != ESP_OK) {
        return ESP_FAIL;
    }

    if (send_chunk(req,
            "<label>WiFi Password</label>"
            "<input name=wifi_pass type=password autocomplete=off>"
            "<label>Setup Code</label>"
            "<input name=claim_code placeholder=ABCD-EFGH required autocomplete=off>"
            "<button type=submit>Configure Gateway</button>"
            "</form></div>") != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_provision_page(httpd_req_t *req, int status, const char *card_class,
                                     const char *title, const char *body)
{
    int n = snprintf(s_card_buf, sizeof(s_card_buf), "<div class=\"c %s\"><h1>%s</h1>%s</div>",
                     card_class ? card_class : "", title, body ? body : "");
    if (n < 0 || (size_t)n >= sizeof(s_card_buf)) {
        ESP_LOGW(TAG, "card HTML truncated (%d bytes)", n);
        return send_provision_shell(req, 500,
                                      "<div class=c><h1>Error</h1>"
                                      "<p class=s>Response too large.</p></div>");
    }
    return send_provision_shell(req, status, s_card_buf);
}

typedef struct {
    char data[HTTP_RESP_BUF_SIZE];
    size_t len;
} http_resp_t;

static esp_err_t claim_http_event(esp_http_client_event_t *evt)
{
    http_resp_t *resp = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp != NULL && evt->data_len > 0) {
        size_t copy = evt->data_len;
        if (resp->len + copy >= sizeof(resp->data)) {
            copy = sizeof(resp->data) - resp->len - 1;
        }
        if (copy > 0) {
            memcpy(resp->data + resp->len, evt->data, copy);
            resp->len += copy;
            resp->data[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *start = strstr(json, needle);
    if (start == NULL) {
        return false;
    }

    start += strlen(needle);
    const char *end = strchr(start, '"');
    if (end == NULL) {
        return false;
    }

    size_t len = (size_t)(end - start);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static esp_err_t claim_gateway(const char *claim_code, char *api_key_out, size_t key_len,
                               char *api_url_out, size_t url_len, char *err_out, size_t err_len)
{
    char body[128];
    const nodifyr_config_t *cfg = nodifyr_config_get();

    snprintf(body, sizeof(body),
             "{\"claim_code\":\"%s\",\"hardware_id\":\"%s\"}",
             claim_code, cfg->hardware_id);

    http_resp_t resp = {0};
    esp_http_client_config_t http_cfg = {
        .url = CONFIG_NODIFYR_CLAIM_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = claim_http_event,
        .user_data = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        snprintf(err_out, err_len, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        snprintf(err_out, err_len, "Network error: %s", esp_err_to_name(err));
        return err;
    }

    if (status == 404) {
        snprintf(err_out, err_len, "Invalid setup code");
        return ESP_FAIL;
    }
    if (status == 409) {
        snprintf(err_out, err_len, "Already claimed or hardware ID taken");
        return ESP_FAIL;
    }
    if (status == 410) {
        snprintf(err_out, err_len, "Setup code expired");
        return ESP_FAIL;
    }
    if (status < 200 || status >= 300) {
        snprintf(err_out, err_len, "Claim failed (HTTP %d)", status);
        return ESP_FAIL;
    }

    if (!json_get_string(resp.data, "api_key", api_key_out, key_len) ||
        !json_get_string(resp.data, "api_url", api_url_out, url_len)) {
        snprintf(err_out, err_len, "Missing api_key in response");
        return ESP_FAIL;
    }

    if (!nodifyr_config_valid_api_key(api_key_out)) {
        snprintf(err_out, err_len, "Invalid API key from server");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t configure_post_handler(httpd_req_t *req)
{
    char body[FORM_BUF_SIZE] = {0};
    int total = 0;

    if (req->content_len >= (int)sizeof(body)) {
        return send_provision_page(req, 500, "err", "Error",
                                   "<p class=s>Form too large.</p>"
                                   "<a class=link href='/'>Try again</a>");
    }

    while (total < req->content_len) {
        int r = httpd_req_recv(req, body + total, req->content_len - total);
        if (r <= 0) {
            return send_provision_page(req, 500, "err", "Error",
                                       "<p class=s>Could not read form.</p>"
                                       "<a class=link href='/'>Try again</a>");
        }
        total += r;
    }
    body[total] = '\0';

    char ssid[NODIFYR_WIFI_SSID_MAX] = {0};
    char ssid_other[NODIFYR_WIFI_SSID_MAX] = {0};
    char pass[NODIFYR_WIFI_PASS_MAX] = {0};
    char claim_code[16] = {0};

    form_get_value(body, "ssid", ssid, sizeof(ssid));
    form_get_value(body, "ssid_other", ssid_other, sizeof(ssid_other));
    form_get_value(body, "wifi_pass", pass, sizeof(pass));

    /* A typed-in network (hidden or not in the scan) overrides the dropdown. */
    if (ssid_other[0] != '\0') {
        strncpy(ssid, ssid_other, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
    }

    if (ssid[0] == '\0' ||
        !form_get_value(body, "claim_code", claim_code, sizeof(claim_code))) {
        return send_provision_page(req, 500, "err", "Error",
                                   "<p class=s>Missing required fields.</p>"
                                   "<a class=link href='/'>Try again</a>");
    }
    normalize_claim_code(claim_code);

    /* Persist creds now so the STA path (after the AP drops) can read them. */
    esp_err_t err = nodifyr_config_save_wifi(ssid, pass);
    if (err != ESP_OK) {
        return send_provision_page(req, 500, "err", "Error",
                                   "<p class=s>Could not save WiFi settings.</p>"
                                   "<a class=link href='/'>Try again</a>");
    }

    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
    strncpy(s_pending_claim, claim_code, sizeof(s_pending_claim) - 1);
    s_pending_claim[sizeof(s_pending_claim) - 1] = '\0';

    ESP_LOGI(TAG, "configure ssid=%s claim=%s (deferred)", ssid, claim_code);
    provision_oled_show("Setup received", ssid, "Connecting...");

    /* Connecting requires tearing the SoftAP down, which kills this socket, so
     * the actual work is handed to provision_run() once the reply is sent. */
    esp_err_t rc = send_provision_page(req, 200, "ok", "Setup Received",
                                       "<p class=s>Your gateway is connecting to the "
                                       "network now. This setup Wi-Fi will turn off "
                                       "&mdash; watch the device screen for status. "
                                       "You can close this page.</p>");
    s_config_ready = true;
    return rc;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (send_shell_open(req, 200) != ESP_OK) {
        return ESP_FAIL;
    }
    if (send_provision_form(req) != ESP_OK) {
        return ESP_FAIL;
    }
    return send_shell_close(req);
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, CAPTIVE_PORTAL_BODY, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_err_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return captive_redirect_handler(req);
}

static void provision_set_dhcp_captive_portal(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif == NULL) {
        ESP_LOGW(TAG, "AP netif missing, skipping DHCP captive portal URI");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGW(TAG, "no AP IP yet; skipping DHCP captive portal URI");
        return;
    }

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));
    snprintf(s_captive_portal_uri, sizeof(s_captive_portal_uri), "http://%s", ip_addr);

    /* Best-effort: failure here only means no auto-popup on some phones. */
    esp_netif_dhcps_stop(netif);
    esp_err_t err = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           s_captive_portal_uri, strlen(s_captive_portal_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP option 114 failed: %s", esp_err_to_name(err));
    }
    esp_netif_dhcps_start(netif);
    ESP_LOGI(TAG, "DHCP captive portal URI: %s", s_captive_portal_uri);
}

static esp_err_t start_portal(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    /* The /configure handler performs an HTTPS claim (mbedTLS handshake) on
     * this task's stack, so it needs far more than the 4 KB default. */
    config.stack_size = 10240;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return ESP_FAIL;
    }

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t configure = {.uri = "/configure", .method = HTTP_POST, .handler = configure_post_handler};
    httpd_uri_t android204 = {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler};
    httpd_uri_t android204b = {.uri = "/gen_204", .method = HTTP_GET, .handler = captive_redirect_handler};
    httpd_uri_t ios_detect = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler};
    httpd_uri_t ios_success = {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = captive_redirect_handler};
    httpd_uri_t win_test = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_handler};
    httpd_uri_t win_ncsi = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_handler};
    httpd_uri_t win_redirect = {.uri = "/redirect", .method = HTTP_GET, .handler = captive_redirect_handler};

    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &configure);
    httpd_register_uri_handler(s_server, &android204);
    httpd_register_uri_handler(s_server, &android204b);
    httpd_register_uri_handler(s_server, &ios_detect);
    httpd_register_uri_handler(s_server, &ios_success);
    httpd_register_uri_handler(s_server, &win_test);
    httpd_register_uri_handler(s_server, &win_ncsi);
    httpd_register_uri_handler(s_server, &win_redirect);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_err_handler);
    return ESP_OK;
}

static void ap_ssid_name(char *ssid_out, size_t ssid_len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid_out, ssid_len, "Nodifyr-Setup-%02X%02X", mac[4], mac[5]);
}

bool provision_boot_button_held(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_NODIFYR_PROVISION_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    if (gpio_get_level(CONFIG_NODIFYR_PROVISION_BUTTON_GPIO) != 0) {
        return false;
    }

    ESP_LOGI(TAG, "BOOT button held — enter provisioning in 5s...");
    provision_oled_show("Hold BOOT...", "Provisioning", "5 seconds");

    for (int i = 0; i < 50; i++) {
        if (gpio_get_level(CONFIG_NODIFYR_PROVISION_BUTTON_GPIO) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "forced provisioning via BOOT button");
    return true;
}

static void serial_monitor_task(void *arg)
{
    char line[24];
    int pos = 0;

    /* Non-blocking stdin so getchar() returns EOF when idle; otherwise a
     * blocking read would pin this task and defeat the timed window. */
    int fd_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fd_flags != -1) {
        fcntl(STDIN_FILENO, F_SETFL, fd_flags | O_NONBLOCK);
    }

    /* Listen for ~30 s after boot, then free the task. */
    for (int i = 0; i < 300; i++) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (c == '\n' || c == '\r') {
            line[pos] = '\0';
            if (strcmp(line, "provision") == 0) {
                s_serial_provision = true;
                ESP_LOGI(TAG, "serial command: provision (reboot to apply)");
            }
            pos = 0;
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)c;
        }
    }

    vTaskDelete(NULL);
}

void provision_serial_monitor_start(void)
{
    if (xTaskCreate(serial_monitor_task, "serial_prov", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "serial monitor not started (low memory)");
    }
}

bool provision_serial_requested(void)
{
    return s_serial_provision;
}

/* Runs on the provisioning task once the user submits the form. Tears the
 * portal + SoftAP down, joins the user's network on a clean STA link (no AP to
 * pin the radio to one channel), claims the gateway, and reboots. Status is
 * shown on the OLED since the phone loses the AP the moment we switch. */
static esp_err_t provision_finish(void)
{
    /* Give the confirmation page time to reach the phone before the AP drops. */
    vTaskDelay(pdMS_TO_TICKS(1200));

    if (s_dns != NULL) {
        stop_dns_server(s_dns);
        s_dns = NULL;
    }
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    provision_oled_show("Connecting WiFi", s_pending_ssid, s_pending_claim);
    ESP_LOGI(TAG, "portal closed; joining %s on STA", s_pending_ssid);

    wifi_stop_ap();

    esp_err_t err = wifi_start_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA start failed: %s", esp_err_to_name(err));
        provision_oled_show("WiFi error", "Rebooting...", "Retry setup");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    if (!wifi_wait_connected(pdMS_TO_TICKS(30000))) {
        ESP_LOGW(TAG, "could not join %s", s_pending_ssid);
        provision_oled_show("WiFi failed", "Check password", "Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    provision_oled_show("Claiming gateway", s_pending_claim, "Please wait...");
    char api_key[NODIFYR_API_KEY_MAX] = {0};
    char api_url[NODIFYR_API_URL_MAX] = {0};
    char err_msg[96] = {0};
    err = claim_gateway(s_pending_claim, api_key, sizeof(api_key),
                        api_url, sizeof(api_url), err_msg, sizeof(err_msg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claim failed: %s", err_msg);
        provision_oled_show("Claim failed", err_msg, "Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(4000));
        esp_restart();
    }

    err = nodifyr_config_save_gateway(api_key, api_url);
    if (err != ESP_OK) {
        provision_oled_show("Save failed", "Rebooting...", NULL);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    char preview[NODIFYR_API_KEY_MAX];
    nodifyr_config_key_preview(api_key, preview, sizeof(preview));
    ESP_LOGI(TAG, "claimed ok key=%s", preview);

    provision_oled_show("Success!", "Rebooting...", "BLE scan next");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

esp_err_t provision_run(void)
{
    char ap_ssid[32];
    ap_ssid_name(ap_ssid, sizeof(ap_ssid));

    /* The scan kicked off during boot may still be running; make sure it has
     * finished (and freed the radio) before we claim it for the SoftAP. This is
     * a no-op if main already started it and it's done. The timeout is well
     * above a normal all-channel scan so the scan's own esp_wifi_stop() always
     * lands before we start the AP. */
    wifi_scan_start();
    wifi_scan_wait(pdMS_TO_TICKS(8000));

    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    provision_oled_show(ap_ssid, "192.168.4.1", "Open network");
    ESP_LOGI(TAG, "SoftAP %s (open, no password)", ap_ssid);

    esp_err_t err = wifi_start_ap(ap_ssid, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP failed (%s); skipping provisioning", esp_err_to_name(err));
        provision_oled_show("Setup AP failed", "Running BLE only", NULL);
        nodifyr_config_disable_auto_setup();
        return err;
    }

    provision_set_dhcp_captive_portal();

    err = start_portal();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal failed (%s); skipping provisioning", esp_err_to_name(err));
        provision_oled_show("Portal failed", "Running BLE only", NULL);
        wifi_stop_ap();
        nodifyr_config_disable_auto_setup();
        return err;
    }

    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);
    if (s_dns == NULL) {
        ESP_LOGW(TAG, "DNS redirect server failed to start (manual nav still works)");
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PROVISION_TIMEOUT_SEC * 1000);
    while (xTaskGetTickCount() < deadline) {
        if (s_config_ready) {
            return provision_finish();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGW(TAG, "provisioning timeout — rebooting to BLE-only");
    nodifyr_config_disable_auto_setup();
    provision_oled_show("Timeout", "Rebooting...", "Hold BOOT to retry");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}
