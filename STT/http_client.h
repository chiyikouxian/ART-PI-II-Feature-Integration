/*
 * http_client.h - 轻量级HTTP客户端(基于SAL socket)
 */
#ifndef __HTTP_CLIENT_H__
#define __HTTP_CLIENT_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP响应结构 */
typedef struct {
    int      status_code;       /* HTTP状态码 */
    char    *body;              /* 响应体(内部分配，调用者需rt_free) */
    uint32_t body_len;          /* 响应体长度 */
} http_response_t;

/**
 * @brief 发送HTTP GET请求
 * @param host      主机名
 * @param port      端口
 * @param path      请求路径(含query string)
 * @param resp      输出: 响应结构
 * @return RT_EOK成功
 */
rt_err_t http_get(const char *host, uint16_t port,
                  const char *path, http_response_t *resp);

/**
 * @brief 发送HTTP POST请求(JSON body)
 * @param host      主机名
 * @param port      端口
 * @param path      请求路径
 * @param body      POST body数据
 * @param body_len  POST body长度
 * @param content_type  Content-Type头
 * @param resp      输出: 响应结构
 * @return RT_EOK成功
 */
rt_err_t http_post(const char *host, uint16_t port,
                   const char *path,
                   const uint8_t *body, uint32_t body_len,
                   const char *content_type,
                   http_response_t *resp);

/**
 * @brief 释放HTTP响应资源
 */
void http_response_free(http_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* __HTTP_CLIENT_H__ */
