/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-10-16     AI Assistant first version - Web Client Implementation
 */

#include <rtthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "web_client.h"

#define DBG_TAG "web.client"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define HTTP_BUFFER_SIZE    (1024)
#define HTTP_RESPONSE_MAX   (1024)  /* STT JSON response is very small, e.g. {"result":["你好"]} */

/* Static receive buffer - avoids malloc when heap is exhausted after large audio send */
static char s_recv_buffer[HTTP_RESPONSE_MAX];

/* ����URL */
static int parse_url(const char *url, char *host, int *port, char *path)
{
    const char *p = url;
    char *h = host;
    
    /* ����Э�� */
    if (strncmp(p, "http://", 7) == 0)
    {
        p += 7;
        *port = 80;
    }
    else if (strncmp(p, "https://", 8) == 0)
    {
        p += 8;
        *port = 443;  /* HTTPS��׼�˿� */
        /* ע�⣺��ǰδ����TLS��HTTPS���˻�ΪHTTP������ȫ�������ڲ��ԣ�*/
        LOG_W("TLS not enabled, HTTPS will work as HTTP (insecure!)");
    }
    else
    {
        *port = 80;
    }
    
    /* ��ȡ������ */
    while (*p && *p != ':' && *p != '/')
    {
        *h++ = *p++;
    }
    *h = '\0';
    
    /* ��ȡ�˿� */
    if (*p == ':')
    {
        p++;
        *port = atoi(p);
        while (*p && *p != '/')
            p++;
    }
    
    /* ��ȡ·�� */
    if (*p == '/')
    {
        strcpy(path, p);
    }
    else
    {
        strcpy(path, "/");
    }
    
    return RT_EOK;
}

/* HTTP GET���� */
int web_client_get(const char *url, http_response_t *response)
{
    int sock = -1;
    struct hostent *host_entry;
    struct sockaddr_in server_addr;
    char host[128] = {0};
    char path[256] = {0};
    char request[512] = {0};
    int port = 80;
    int ret = -RT_ERROR;
    int recv_len = 0;
    int total_len = 0;

    if (url == RT_NULL || response == RT_NULL)
    {
        return -RT_EINVAL;
    }
    
    rt_memset(response, 0, sizeof(http_response_t));
    
    /* ����URL */
    if (parse_url(url, host, &port, path) != RT_EOK)
    {
        LOG_E("Failed to parse URL: %s", url);
        return -RT_ERROR;
    }
    
    LOG_D("Connecting to %s:%d%s", host, port, path);
    
    /* �������� */
    host_entry = gethostbyname(host);
    if (host_entry == RT_NULL)
    {
        LOG_E("Failed to resolve host: %s", host);
        return -RT_ERROR;
    }
    
    /* ����socket */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        LOG_E("Failed to create socket");
        return -RT_ERROR;
    }
    
    /* ���ó�ʱ */
    struct timeval timeout = {10, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    /* ���ӷ����� */
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = *((struct in_addr *)host_entry->h_addr);
    rt_memset(&(server_addr.sin_zero), 0, sizeof(server_addr.sin_zero));
    
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) < 0)
    {
        LOG_E("Failed to connect to server");
        closesocket(sock);
        return -RT_ERROR;
    }
    
    /* ����HTTP GET���� */
    rt_snprintf(request, sizeof(request),
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: RT-Thread\r\n"
                "Connection: close\r\n"
                "\r\n",
                path, host);
    
    /* �������� */
    if (send(sock, request, strlen(request), 0) < 0)
    {
        LOG_E("Failed to send request");
        closesocket(sock);
        return -RT_ERROR;
    }
    
    /* 使用静态接收缓冲区（避免 malloc 失败） */
    char *recv_buffer = s_recv_buffer;

    /* ������Ӧ */
    while ((recv_len = recv(sock, recv_buffer + total_len, HTTP_RESPONSE_MAX - total_len - 1, 0)) > 0)
    {
        total_len += recv_len;
        if (total_len >= HTTP_RESPONSE_MAX - 1)
        {
            LOG_W("Response too large, truncated");
            break;
        }
    }

    recv_buffer[total_len] = '\0';

    if (total_len > 0)
    {
        /* ����HTTP��Ӧ */
        char *status_line = recv_buffer;
        char *header_end = strstr(recv_buffer, "\r\n\r\n");

        if (header_end)
        {
            /* ��ȡ״̬�� */
            sscanf(status_line, "HTTP/1.%*d %d", &response->status_code);

            /* ��ȡ��Ӧ�� */
            char *body_start = header_end + 4;
            response->body_len = total_len - (body_start - recv_buffer);
            response->body = (char *)rt_malloc(response->body_len + 1);
            if (response->body)
            {
                rt_memcpy(response->body, body_start, response->body_len);
                response->body[response->body_len] = '\0';
            }

            LOG_D("HTTP Response: status=%d, body_len=%d",
                  response->status_code, response->body_len);

            ret = RT_EOK;
        }
    }

    closesocket(sock);

    return ret;
}

/* HTTP POST���� */
int web_client_post(const char *url, const char *data, uint32_t data_len,
                    const char *content_type, http_response_t *response)
{
    int sock = -1;
    struct hostent *host_entry;
    struct sockaddr_in server_addr;
    char host[128] = {0};
    char path[256] = {0};
    char *recv_buffer = RT_NULL;
    int port = 80;
    int ret = -RT_ERROR;
    int recv_len = 0;
    int total_len = 0;
    
    if (url == RT_NULL || data == RT_NULL || response == RT_NULL)
    {
        return -RT_EINVAL;
    }
    
    rt_memset(response, 0, sizeof(http_response_t));
    
    /* ����URL */
    if (parse_url(url, host, &port, path) != RT_EOK)
    {
        LOG_E("Failed to parse URL: %s", url);
        return -RT_ERROR;
    }
    
    LOG_D("Connecting to %s:%d%s", host, port, path);
    
    /* �������� */
    host_entry = gethostbyname(host);
    if (host_entry == RT_NULL)
    {
        LOG_E("Failed to resolve host: %s", host);
        return -RT_ERROR;
    }
    
    /* ����socket */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        LOG_E("Failed to create socket");
        return -RT_ERROR;
    }
    
    /* ���ó�ʱ */
    struct timeval timeout = {30, 0};  /* POST���������Ҫ����ʱ�� */
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    /* ���ӷ����� */
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = *((struct in_addr *)host_entry->h_addr);
    rt_memset(&(server_addr.sin_zero), 0, sizeof(server_addr.sin_zero));
    
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) < 0)
    {
        LOG_E("Failed to connect to server");
        closesocket(sock);
        return -RT_ERROR;
    }
    
    /* 构建 HTTP header（仅分配 header 空间，不复制 body） */
    {
        char header[512];
        int header_len = rt_snprintf(header, sizeof(header),
                                      "POST %s HTTP/1.1\r\n"
                                      "Host: %s\r\n"
                                      "User-Agent: RT-Thread\r\n"
                                      "Content-Type: %s\r\n"
                                      "Content-Length: %d\r\n"
                                      "Connection: close\r\n"
                                      "\r\n",
                                      path, host, content_type ? content_type : "application/octet-stream", data_len);

        /* 先发送 HTTP header */
        if (send(sock, header, header_len, 0) < 0)
        {
            LOG_E("Failed to send HTTP header");
            closesocket(sock);
            return -RT_ERROR;
        }
    }

    /* 再分块发送 body 数据（直接从调用者缓冲区发送，零拷贝） */
    {
        uint32_t sent = 0;
        while (sent < data_len)
        {
            uint32_t chunk = data_len - sent;
            if (chunk > 4096) chunk = 4096;  /* 每次最多发 4KB */
            int n = send(sock, data + sent, chunk, 0);
            if (n < 0)
            {
                LOG_W("Send body stopped at offset %d/%d (server may have enough data)",
                      sent, data_len);
                break;  /* 不立即返回错误，继续尝试读取响应 */
            }
            sent += n;
        }
        LOG_D("Sent %d/%d bytes body data", sent, data_len);
    }
    
    /* 使用静态接收缓冲区（避免 malloc 失败） */
    recv_buffer = s_recv_buffer;

    /* ������Ӧ */
    while ((recv_len = recv(sock, recv_buffer + total_len, HTTP_RESPONSE_MAX - total_len - 1, 0)) > 0)
    {
        total_len += recv_len;
        if (total_len >= HTTP_RESPONSE_MAX - 1)
        {
            LOG_W("Response too large, truncated");
            break;
        }
    }

    recv_buffer[total_len] = '\0';

    if (total_len > 0)
    {
        /* ����HTTP��Ӧ */
        char *status_line = recv_buffer;
        char *header_end = strstr(recv_buffer, "\r\n\r\n");

        if (header_end)
        {
            /* ��ȡ״̬�� */
            sscanf(status_line, "HTTP/1.%*d %d", &response->status_code);

            /* ��ȡ��Ӧ�� */
            char *body_start = header_end + 4;
            response->body_len = total_len - (body_start - recv_buffer);
            response->body = (char *)rt_malloc(response->body_len + 1);
            if (response->body)
            {
                rt_memcpy(response->body, body_start, response->body_len);
                response->body[response->body_len] = '\0';
            }

            LOG_D("HTTP Response: status=%d, body_len=%d",
                  response->status_code, response->body_len);

            ret = RT_EOK;
        }
    }

    closesocket(sock);

    return ret;
}

/* HTTP POST���󣨴��Զ���Header��*/
int web_client_post_with_header(const char *url, const char *data, uint32_t data_len,
                                  const char *content_type, const char *custom_header,
                                  http_response_t *response)
{
    int sock = -1;
    struct hostent *host_entry;
    struct sockaddr_in server_addr;
    char host[128] = {0};
    char path[256] = {0};
    char *request = RT_NULL;
    char *recv_buffer = RT_NULL;
    int port = 80;
    int ret = -RT_ERROR;
    int recv_len = 0;
    int total_len = 0;
    
    if (url == RT_NULL || data == RT_NULL || response == RT_NULL)
    {
        return -RT_EINVAL;
    }
    
    rt_memset(response, 0, sizeof(http_response_t));
    
    /* ����URL */
    if (parse_url(url, host, &port, path) != RT_EOK)
    {
        LOG_E("Failed to parse URL: %s", url);
        return -RT_ERROR;
    }
    
    LOG_D("Connecting to %s:%d%s", host, port, path);
    
    /* �������� */
    host_entry = gethostbyname(host);
    if (host_entry == RT_NULL)
    {
        LOG_E("Failed to resolve host: %s", host);
        return -RT_ERROR;
    }
    
    /* ����socket */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        LOG_E("Failed to create socket");
        return -RT_ERROR;
    }
    
    /* ���ó�ʱ */
    struct timeval timeout = {30, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    /* ���ӷ����� */
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = *((struct in_addr *)host_entry->h_addr);
    rt_memset(&(server_addr.sin_zero), 0, sizeof(server_addr.sin_zero));
    
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) < 0)
    {
        LOG_E("Failed to connect to server");
        closesocket(sock);
        return -RT_ERROR;
    }
    
    /* �������󻺳��� */
    request = (char *)rt_malloc(1024 + data_len);
    if (request == RT_NULL)
    {
        LOG_E("Failed to allocate request buffer (%d bytes)", 1024 + data_len);
        closesocket(sock);
        return -RT_ERROR;
    }
    
    /* ����HTTP POST���󣨴��Զ���Header��*/
    int header_len = rt_snprintf(request, 1024,
                                  "POST %s HTTP/1.1\r\n"
                                  "Host: %s\r\n"
                                  "User-Agent: RT-Thread\r\n"
                                  "Content-Type: %s\r\n"
                                  "Content-Length: %d\r\n"
                                  "%s"  /* �Զ���Header */
                                  "Connection: close\r\n"
                                  "\r\n",
                                  path, host, content_type ? content_type : "application/json", data_len,
                                  custom_header ? custom_header : "");
    
    /* �������� */
    rt_memcpy(request + header_len, data, data_len);
    
    LOG_D("Request headers:\n%.*s", header_len, request);
    
    /* �������� */
    if (send(sock, request, header_len + data_len, 0) < 0)
    {
        LOG_E("Failed to send request");
        rt_free(request);
        closesocket(sock);
        return -RT_ERROR;
    }
    
    rt_free(request);
    
    /* 使用静态接收缓冲区（避免 malloc 失败） */
    recv_buffer = s_recv_buffer;

    /* ������Ӧ */
    while ((recv_len = recv(sock, recv_buffer + total_len, HTTP_RESPONSE_MAX - total_len - 1, 0)) > 0)
    {
        total_len += recv_len;
        if (total_len >= HTTP_RESPONSE_MAX - 1)
        {
            LOG_W("Response too large, truncated");
            break;
        }
    }

    recv_buffer[total_len] = '\0';

    if (total_len > 0)
    {
        /* ����HTTP��Ӧ */
        char *status_line = recv_buffer;
        char *header_end = strstr(recv_buffer, "\r\n\r\n");

        if (header_end)
        {
            /* ��ȡ״̬�� */
            sscanf(status_line, "HTTP/1.%*d %d", &response->status_code);

            /* ��ȡ��Ӧ�� */
            char *body_start = header_end + 4;
            response->body_len = total_len - (body_start - recv_buffer);
            response->body = (char *)rt_malloc(response->body_len + 1);
            if (response->body)
            {
                rt_memcpy(response->body, body_start, response->body_len);
                response->body[response->body_len] = '\0';
            }

            LOG_D("HTTP Response: status=%d, body_len=%d",
                  response->status_code, response->body_len);

            ret = RT_EOK;
        }
    }

    closesocket(sock);

    return ret;
}

/* �ϴ��ļ���multipart/form-data��*/
int web_client_post_file(const char *url, const uint8_t *file_data, uint32_t file_len,
                          const char *field_name, const char *file_name,
                          http_response_t *response)
{
    const char *boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    char *multipart_data = RT_NULL;
    int multipart_len = 0;
    int ret = -RT_ERROR;
    char content_type[128];
    
    if (url == RT_NULL || file_data == RT_NULL || response == RT_NULL)
    {
        return -RT_EINVAL;
    }
    
    /* ����multipart���ݴ�С */
    multipart_len = strlen(boundary) * 2 + strlen(field_name) + strlen(file_name) + file_len + 256;
    
    /* ���仺���� */
    multipart_data = (char *)rt_malloc(multipart_len);
    if (multipart_data == RT_NULL)
    {
        LOG_E("Failed to allocate multipart buffer");
        return -RT_ERROR;
    }
    
    /* ����multipart���� */
    int offset = 0;
    offset += rt_snprintf(multipart_data + offset, multipart_len - offset,
                          "--%s\r\n"
                          "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                          "Content-Type: application/octet-stream\r\n"
                          "\r\n",
                          boundary, field_name, file_name);
    
    rt_memcpy(multipart_data + offset, file_data, file_len);
    offset += file_len;
    
    offset += rt_snprintf(multipart_data + offset, multipart_len - offset,
                          "\r\n--%s--\r\n", boundary);
    
    /* ����Content-Type */
    rt_snprintf(content_type, sizeof(content_type), 
                "multipart/form-data; boundary=%s", boundary);
    
    /* ����POST���� */
    ret = web_client_post(url, multipart_data, offset, content_type, response);
    
    rt_free(multipart_data);
    
    return ret;
}

/* �ͷ���Ӧ���� */
void web_client_free_response(http_response_t *response)
{
    if (response)
    {
        if (response->body)
        {
            rt_free(response->body);
            response->body = RT_NULL;
        }
        if (response->content_type)
        {
            rt_free(response->content_type);
            response->content_type = RT_NULL;
        }
        response->body_len = 0;
        response->status_code = 0;
    }
}

