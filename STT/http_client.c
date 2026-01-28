/*
 * http_client.c - 轻量级HTTP客户端(基于SAL/POSIX socket)
 */
#include "http_client.h"
#include "stt_config.h"
#include <string.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netdb.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

/* 内部: 创建TCP连接 */
static int http_connect(const char *host, uint16_t port)
{
    struct hostent *he;
    struct sockaddr_in server_addr;
    int sock;
    struct timeval tv;
    int opt;

    rt_kprintf("[HTTP] Connecting to %s:%d\n", host, port);

    he = gethostbyname(host);
    if (he == RT_NULL)
    {
        rt_kprintf("[HTTP] DNS resolve failed: %s\n", host);
        return -1;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        rt_kprintf("[HTTP] Socket create failed\n");
        return -1;
    }

    /* 设置发送/接收超时 */
    tv.tv_sec  = HTTP_SEND_TIMEOUT;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    tv.tv_sec  = HTTP_RECV_TIMEOUT;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 禁用Nagle算法，减少小包延迟 */
    opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    /* 设置发送缓冲区大小 */
    opt = 8192;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt));

    rt_memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(port);
    server_addr.sin_addr   = *((struct in_addr *)he->h_addr);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        rt_kprintf("[HTTP] Connect failed: %s:%d\n", host, port);
        closesocket(sock);
        return -1;
    }

    rt_kprintf("[HTTP] Connected successfully\n");
    return sock;
}

/* 内部: 发送全部数据 (分块发送大数据) */
static rt_err_t http_send_all(int sock, const void *data, uint32_t len)
{
    const uint8_t *ptr = (const uint8_t *)data;
    uint32_t sent = 0;
    int retry_count = 0;
    const int max_retries = 3;
    const uint32_t chunk_size = 512;  /* 每次发送512字节，减少内存压力 */

    while (sent < len)
    {
        uint32_t to_send = len - sent;
        if (to_send > chunk_size)
            to_send = chunk_size;

        int ret = send(sock, ptr + sent, to_send, 0);
        if (ret <= 0)
        {
            retry_count++;
            if (retry_count >= max_retries)
            {
                rt_kprintf("[HTTP] Send failed at %d/%d after %d retries\n", sent, len, max_retries);
                return -RT_ERROR;
            }
            rt_kprintf("[HTTP] Send retry %d at %d/%d\n", retry_count, sent, len);
            rt_thread_mdelay(200);  /* 等待200ms后重试，让WiFi驱动释放内存 */
            continue;
        }
        sent += ret;
        retry_count = 0;  /* 成功发送后重置重试计数 */

        /* 每发送一块后短暂延迟，减轻WiFi驱动内存压力 */
        if (sent < len)
        {
            rt_thread_mdelay(5);  /* 5ms间隔 */
        }

        /* 每发送10KB打印一次进度 */
        if ((sent % 10240) == 0 || sent == len)
        {
            uint32_t percent = (sent * 100) / len;
            rt_kprintf("[HTTP] Sent %d/%d bytes (%d%%)\n", sent, len, percent);
        }
    }
    return RT_EOK;
}

/* 内部: 接收HTTP响应并解析 */
static rt_err_t http_recv_response(int sock, http_response_t *resp)
{
    char *recv_buf;
    int total_len = 0;
    int ret;
    int buf_capacity = HTTP_RECV_BUF_SIZE;

    recv_buf = (char *)rt_malloc(buf_capacity);
    if (recv_buf == RT_NULL)
        return -RT_ENOMEM;

    /* 接收数据 */
    while (1)
    {
        if (total_len >= buf_capacity - 1)
        {
            /* 扩容 */
            int new_cap = buf_capacity + HTTP_RECV_BUF_SIZE;
            char *new_buf = rt_realloc(recv_buf, new_cap);
            if (new_buf == RT_NULL)
                break;
            recv_buf = new_buf;
            buf_capacity = new_cap;
        }

        ret = recv(sock, recv_buf + total_len, buf_capacity - total_len - 1, 0);
        if (ret <= 0)
            break;
        total_len += ret;
    }

    recv_buf[total_len] = '\0';

    if (total_len == 0)
    {
        rt_free(recv_buf);
        return -RT_ERROR;
    }

    /* 解析状态码: "HTTP/1.x NNN" */
    resp->status_code = 0;
    char *status_start = strstr(recv_buf, "HTTP/");
    if (status_start)
    {
        char *code_start = strchr(status_start, ' ');
        if (code_start)
            resp->status_code = atoi(code_start + 1);
    }

    /* 找到body (空行 \r\n\r\n 之后) */
    char *body_start = strstr(recv_buf, "\r\n\r\n");
    if (body_start)
    {
        body_start += 4;
        resp->body_len = total_len - (body_start - recv_buf);
        resp->body = (char *)rt_malloc(resp->body_len + 1);
        if (resp->body)
        {
            rt_memcpy(resp->body, body_start, resp->body_len);
            resp->body[resp->body_len] = '\0';
        }
    }
    else
    {
        resp->body = RT_NULL;
        resp->body_len = 0;
    }

    rt_free(recv_buf);
    return RT_EOK;
}

rt_err_t http_get(const char *host, uint16_t port,
                  const char *path, http_response_t *resp)
{
    int sock;
    char *request;
    int req_len;
    rt_err_t ret;

    rt_memset(resp, 0, sizeof(http_response_t));

    sock = http_connect(host, port);
    if (sock < 0)
        return -RT_ERROR;

    /* 构造GET请求 */
    request = (char *)rt_malloc(512 + rt_strlen(path));
    if (request == RT_NULL)
    {
        closesocket(sock);
        return -RT_ENOMEM;
    }

    req_len = rt_snprintf(request, 512 + rt_strlen(path),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    ret = http_send_all(sock, request, req_len);
    rt_free(request);

    if (ret != RT_EOK)
    {
        closesocket(sock);
        return ret;
    }

    ret = http_recv_response(sock, resp);
    closesocket(sock);
    return ret;
}

rt_err_t http_post(const char *host, uint16_t port,
                   const char *path,
                   const uint8_t *body, uint32_t body_len,
                   const char *content_type,
                   http_response_t *resp)
{
    int sock;
    char *header;
    int hdr_len;
    rt_err_t ret;

    rt_memset(resp, 0, sizeof(http_response_t));

    sock = http_connect(host, port);
    if (sock < 0)
        return -RT_ERROR;

    /* 构造POST头 */
    header = (char *)rt_malloc(512 + rt_strlen(path));
    if (header == RT_NULL)
    {
        closesocket(sock);
        return -RT_ENOMEM;
    }

    hdr_len = rt_snprintf(header, 512 + rt_strlen(path),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, content_type, body_len);

    /* 发送头 */
    ret = http_send_all(sock, header, hdr_len);
    rt_free(header);

    if (ret != RT_EOK)
    {
        closesocket(sock);
        return ret;
    }

    /* 发送body */
    ret = http_send_all(sock, body, body_len);
    if (ret != RT_EOK)
    {
        closesocket(sock);
        return ret;
    }

    ret = http_recv_response(sock, resp);
    closesocket(sock);
    return ret;
}

void http_response_free(http_response_t *resp)
{
    if (resp && resp->body)
    {
        rt_free(resp->body);
        resp->body = RT_NULL;
        resp->body_len = 0;
    }
}
