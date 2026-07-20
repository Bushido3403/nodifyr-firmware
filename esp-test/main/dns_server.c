/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "dns_server.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#define DNS_PORT (53)
#define DNS_MAX_LEN (256)
#define OPCODE_MASK (0x7800)
#define QR_FLAG (1 << 7)
#define QD_TYPE_A (0x0001)
#define ANS_TTL_SEC (300)

static const char *TAG = "dns_server";

typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

struct dns_server_handle {
    bool started;
    TaskHandle_t task;
    int num_of_entries;
    dns_entry_pair_t entry[];
};

static char *parse_dns_name(char *raw_name, char *parsed_name, size_t parsed_name_max_len)
{
    char *label = raw_name;
    char *name_itr = parsed_name;
    int name_len = 0;

    do {
        int sub_name_len = *label;
        name_len += (sub_name_len + 1);
        if (name_len > (int)parsed_name_max_len) {
            return NULL;
        }

        memcpy(name_itr, label + 1, sub_name_len);
        name_itr[sub_name_len] = '.';
        name_itr += (sub_name_len + 1);
        label += sub_name_len + 1;
    } while (*label != 0);

    parsed_name[name_len - 1] = '\0';
    return label + 1;
}

static int parse_dns_request(char *req, size_t req_len, char *dns_reply, size_t dns_reply_max_len,
                             dns_server_handle_t h)
{
    if (req_len > dns_reply_max_len) {
        return -1;
    }

    memset(dns_reply, 0, dns_reply_max_len);
    memcpy(dns_reply, req, req_len);

    dns_header_t *header = (dns_header_t *)dns_reply;
    if ((header->flags & OPCODE_MASK) != 0) {
        return 0;
    }

    header->flags |= QR_FLAG;

    uint16_t qd_count = ntohs(header->qd_count);
    header->an_count = htons(qd_count);

    int reply_len = qd_count * (int)sizeof(dns_answer_t) + (int)req_len;
    if (reply_len > (int)dns_reply_max_len) {
        return -1;
    }

    char *cur_ans_ptr = dns_reply + req_len;
    char *cur_qd_ptr = dns_reply + sizeof(dns_header_t);
    char name[128];

    for (int qd_i = 0; qd_i < qd_count; qd_i++) {
        char *name_end_ptr = parse_dns_name(cur_qd_ptr, name, sizeof(name));
        if (name_end_ptr == NULL) {
            return -1;
        }

        dns_question_t *question = (dns_question_t *)(name_end_ptr);
        uint16_t qd_type = ntohs(question->type);
        uint16_t qd_class = ntohs(question->class);

        if (qd_type == QD_TYPE_A) {
            esp_ip4_addr_t ip = {.addr = IPADDR_ANY};
            for (int i = 0; i < h->num_of_entries; ++i) {
                if (strcmp(h->entry[i].name, "*") == 0 || strcmp(h->entry[i].name, name) == 0) {
                    if (h->entry[i].if_key) {
                        esp_netif_ip_info_t ip_info;
                        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey(h->entry[i].if_key), &ip_info);
                        ip.addr = ip_info.ip.addr;
                        break;
                    } else if (h->entry[i].ip.addr != IPADDR_ANY) {
                        ip.addr = h->entry[i].ip.addr;
                        break;
                    }
                }
            }
            if (ip.addr == IPADDR_ANY) {
                continue;
            }

            dns_answer_t *answer = (dns_answer_t *)cur_ans_ptr;
            answer->ptr_offset = htons(0xC000 | (cur_qd_ptr - dns_reply));
            answer->type = htons(qd_type);
            answer->class = htons(qd_class);
            answer->ttl = htonl(ANS_TTL_SEC);
            answer->addr_len = htons(sizeof(ip.addr));
            answer->ip_addr = ip.addr;
        }
    }
    return reply_len;
}

static void dns_server_task(void *pvParameters)
{
    dns_server_handle_t handle = pvParameters;

    while (handle->started) {
        struct sockaddr_in dest_addr = {
            .sin_addr.s_addr = htonl(INADDR_ANY),
            .sin_family = AF_INET,
            .sin_port = htons(DNS_PORT),
        };

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket failed: errno %d", errno);
            break;
        }

        if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
            ESP_LOGE(TAG, "bind failed: errno %d", errno);
            close(sock);
            break;
        }

        ESP_LOGI(TAG, "DNS redirect listening on port %d", DNS_PORT);

        while (handle->started) {
            char rx_buffer[128];
            struct sockaddr_in source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr *)&source_addr, &socklen);
            if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                close(sock);
                break;
            }

            char reply[DNS_MAX_LEN];
            int reply_len = parse_dns_request(rx_buffer, len, reply, DNS_MAX_LEN, handle);
            if (reply_len > 0) {
                sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
            }
        }

        close(sock);
    }

    vTaskDelete(NULL);
}

dns_server_handle_t start_dns_server(dns_server_config_t *config)
{
    dns_server_handle_t handle =
        calloc(1, sizeof(struct dns_server_handle) + config->num_of_entries * sizeof(dns_entry_pair_t));
    ESP_RETURN_ON_FALSE(handle, NULL, TAG, "alloc failed");

    handle->started = true;
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry, config->item, config->num_of_entries * sizeof(dns_entry_pair_t));

    if (xTaskCreate(dns_server_task, "dns_server", 4096, handle, 5, &handle->task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        free(handle);
        return NULL;
    }
    return handle;
}

void stop_dns_server(dns_server_handle_t handle)
{
    if (handle) {
        handle->started = false;
        vTaskDelete(handle->task);
        free(handle);
    }
}
