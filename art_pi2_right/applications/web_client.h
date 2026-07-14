/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-10-16     AI Assistant first version - Web Client Module
 */

#ifndef __WEB_CLIENT_H__
#define __WEB_CLIENT_H__

#include <rtthread.h>

/* HTTP响应结构 */
typedef struct {
    int status_code;
    char *body;
    uint32_t body_len;
    char *content_type;
} http_response_t;

/* HTTP请求接口 */
int web_client_get(const char *url, http_response_t *response);
int web_client_post(const char *url, const char *data, uint32_t data_len, 
                    const char *content_type, http_response_t *response);
int web_client_post_with_header(const char *url, const char *data, uint32_t data_len,
                                  const char *content_type, const char *custom_header,
                                  http_response_t *response);
int web_client_post_file(const char *url, const uint8_t *file_data, uint32_t file_len,
                          const char *field_name, const char *file_name,
                          http_response_t *response);
void web_client_free_response(http_response_t *response);

#endif /* __WEB_CLIENT_H__ */

