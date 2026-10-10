/**
 * @file wled_proxy.c
 * @brief Server only: takes TCP connections on a WLED's port and passes
 *        them on to where the WLED is.
 *
 * The server's access point does not carry traffic between two of its own
 * stations (seen 2026-10-10: a laptop on it could not even ping a client on
 * it), and lwIP's port maps do not apply to packets arriving on the NAPT
 * side. The server itself, though, every station in the mesh reaches. So it
 * accepts the connection and opens a second one to the WLED, or to the
 * client below that forwards the port on to it, and copies both ways.
 *
 * Kept small on purpose: at most two connections at a time, 1460-byte
 * buffers in PSRAM, and none while internal RAM runs low -- the server's
 * TCP buffers come from the same internal pool as Wi-Fi's, and all audio
 * stalled once when that ran dry. A browser waits in the listen backlog
 * for a free slot.
 */
#include "wled_proxy.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "WLED_PROXY";

#define MAX_PORTS          4
#define MAX_CONNS          2
#define BUF_SIZE           1460
#define LAPSE_US           (90LL * 1000000LL)
/* Below this much free internal RAM no new connection is taken. */
#define MIN_INTERNAL_FREE  (24 * 1024)
#define TASK_PRIORITY      1
#define TASK_STACK         4096
#define TASK_STACK_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    uint16_t port;          /* 0 = free */
    uint32_t target_ip;
    uint16_t target_port;
    int64_t  until_us;
    int      listen_fd;
} proxy_port_t;

/* One way of a connection: what came in on `from`, still to go to `to`. */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   off;
} pipe_t;

typedef struct {
    bool   used;
    bool   connecting;      /* the outgoing side is not up yet */
    int    in_fd;           /* from the browser */
    int    out_fd;          /* to the WLED or the client below */
    pipe_t up;              /* in -> out */
    pipe_t down;            /* out -> in */
} proxy_conn_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static proxy_port_t s_ports[MAX_PORTS];
static proxy_conn_t s_conns[MAX_CONNS];
static TaskHandle_t s_task;
static bool s_ports_ready;

static void set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_conn(proxy_conn_t *c)
{
    if (c->in_fd >= 0) {
        close(c->in_fd);
    }
    if (c->out_fd >= 0) {
        close(c->out_fd);
    }
    c->used = false;
    c->in_fd = c->out_fd = -1;
    c->up.len = c->up.off = 0;
    c->down.len = c->down.off = 0;
}

/* Opens, renews or closes the listening sockets to match s_ports. */
static void tend_listeners(int64_t now_us)
{
    for (size_t i = 0; i < MAX_PORTS; ++i) {
        portENTER_CRITICAL(&s_lock);
        proxy_port_t p = s_ports[i];
        const bool lapsed = p.port != 0U && now_us >= p.until_us;
        if (lapsed) {
            s_ports[i].port = 0U;
        }
        portEXIT_CRITICAL(&s_lock);

        if ((lapsed || p.port == 0U) && p.listen_fd >= 0) {
            close(p.listen_fd);
            ESP_LOGI(TAG, "Port %u closed", (unsigned)p.port);
            portENTER_CRITICAL(&s_lock);
            s_ports[i].listen_fd = -1;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }
        if (lapsed || p.port == 0U || p.listen_fd >= 0) {
            continue;
        }

        const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            continue;
        }
        const int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(p.port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
            ESP_LOGW(TAG, "Port %u: cannot listen, errno %d", (unsigned)p.port, errno);
            close(fd);
            continue;
        }
        set_nonblocking(fd);
        portENTER_CRITICAL(&s_lock);
        s_ports[i].listen_fd = fd;
        portEXIT_CRITICAL(&s_lock);
        const struct in_addr target = {.s_addr = p.target_ip};
        ESP_LOGI(TAG, "Port %u -> %s:%u", (unsigned)p.port, inet_ntoa(target),
                 (unsigned)p.target_port);
    }
}

static void accept_on(const proxy_port_t *p)
{
    const int in_fd = accept(p->listen_fd, NULL, NULL);
    if (in_fd < 0) {
        return;
    }

    proxy_conn_t *c = NULL;
    for (size_t i = 0; i < MAX_CONNS && c == NULL; ++i) {
        if (!s_conns[i].used) {
            c = &s_conns[i];
        }
    }
    if (c == NULL || heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < MIN_INTERNAL_FREE) {
        close(in_fd);
        return;
    }

    const int out_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (out_fd < 0) {
        close(in_fd);
        return;
    }
    set_nonblocking(in_fd);
    set_nonblocking(out_fd);
    struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port = htons(p->target_port),
        .sin_addr.s_addr = p->target_ip,
    };
    if (connect(out_fd, (struct sockaddr *)&target, sizeof(target)) != 0 && errno != EINPROGRESS) {
        close(in_fd);
        close(out_fd);
        return;
    }
    c->used = true;
    c->connecting = true;
    c->in_fd = in_fd;
    c->out_fd = out_fd;
    c->up.len = c->up.off = 0;
    c->down.len = c->down.off = 0;
}

/* Moves what is there from `from` through `pipe` to `to`. False when the
 * connection is over. */
static bool pump(int from, int to, pipe_t *pipe, bool readable, bool writable)
{
    if (pipe->len == 0U && readable) {
        const int got = recv(from, pipe->buf, BUF_SIZE, 0);
        if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return false;
        }
        if (got > 0) {
            pipe->len = (size_t)got;
            pipe->off = 0;
            writable = true; /* try at once */
        }
    }
    if (pipe->len > 0U && writable) {
        const int sent = send(to, pipe->buf + pipe->off, pipe->len - pipe->off, 0);
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
        if (sent > 0) {
            pipe->off += (size_t)sent;
            if (pipe->off >= pipe->len) {
                pipe->len = pipe->off = 0;
            }
        }
    }
    return true;
}

static void proxy_task(void *arg)
{
    (void)arg;
    for (size_t i = 0; i < MAX_CONNS; ++i) {
        s_conns[i].in_fd = s_conns[i].out_fd = -1;
        s_conns[i].up.buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_conns[i].down.buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_conns[i].up.buf == NULL || s_conns[i].down.buf == NULL) {
            ESP_LOGE(TAG, "No memory for the buffers");
            vTaskSuspend(NULL);
        }
    }

    for (;;) {
        tend_listeners(esp_timer_get_time());

        fd_set rd;
        fd_set wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        int max_fd = -1;
        proxy_port_t ports[MAX_PORTS];
        portENTER_CRITICAL(&s_lock);
        memcpy(ports, s_ports, sizeof(ports));
        portEXIT_CRITICAL(&s_lock);
        for (size_t i = 0; i < MAX_PORTS; ++i) {
            if (ports[i].port != 0U && ports[i].listen_fd >= 0) {
                FD_SET(ports[i].listen_fd, &rd);
                max_fd = (ports[i].listen_fd > max_fd) ? ports[i].listen_fd : max_fd;
            }
        }
        for (size_t i = 0; i < MAX_CONNS; ++i) {
            proxy_conn_t *c = &s_conns[i];
            if (!c->used) {
                continue;
            }
            if (c->connecting) {
                FD_SET(c->out_fd, &wr);
            } else {
                if (c->up.len == 0U) {
                    FD_SET(c->in_fd, &rd);
                } else {
                    FD_SET(c->out_fd, &wr);
                }
                if (c->down.len == 0U) {
                    FD_SET(c->out_fd, &rd);
                } else {
                    FD_SET(c->in_fd, &wr);
                }
            }
            max_fd = (c->in_fd > max_fd) ? c->in_fd : max_fd;
            max_fd = (c->out_fd > max_fd) ? c->out_fd : max_fd;
        }

        if (max_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
        if (select(max_fd + 1, &rd, &wr, NULL, &timeout) <= 0) {
            continue;
        }

        for (size_t i = 0; i < MAX_CONNS; ++i) {
            proxy_conn_t *c = &s_conns[i];
            if (!c->used) {
                continue;
            }
            if (c->connecting) {
                if (!FD_ISSET(c->out_fd, &wr)) {
                    continue;
                }
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(c->out_fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                    close_conn(c);
                    continue;
                }
                c->connecting = false;
                continue;
            }
            const bool ok =
                pump(c->in_fd, c->out_fd, &c->up, FD_ISSET(c->in_fd, &rd), FD_ISSET(c->out_fd, &wr)) &&
                pump(c->out_fd, c->in_fd, &c->down, FD_ISSET(c->out_fd, &rd), FD_ISSET(c->in_fd, &wr));
            if (!ok) {
                close_conn(c);
            }
        }
        for (size_t i = 0; i < MAX_PORTS; ++i) {
            if (ports[i].port != 0U && ports[i].listen_fd >= 0 && FD_ISSET(ports[i].listen_fd, &rd)) {
                accept_on(&ports[i]);
            }
        }
    }
}

void wled_proxy_set(uint16_t port, uint32_t target_ip, uint16_t target_port)
{
    const int64_t until_us = esp_timer_get_time() + LAPSE_US;
    bool stored = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_ports_ready) {
        for (size_t i = 0; i < MAX_PORTS; ++i) {
            s_ports[i].listen_fd = -1;
        }
        s_ports_ready = true;
    }
    proxy_port_t *slot = NULL;
    for (size_t i = 0; i < MAX_PORTS && slot == NULL; ++i) {
        if (s_ports[i].port == port) {
            slot = &s_ports[i];
        }
    }
    for (size_t i = 0; i < MAX_PORTS && slot == NULL; ++i) {
        if (s_ports[i].port == 0U && s_ports[i].listen_fd < 0) {
            slot = &s_ports[i];
        }
    }
    if (slot != NULL) {
        slot->port = port;
        slot->target_ip = target_ip;
        slot->target_port = target_port;
        slot->until_us = until_us;
        stored = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!stored) {
        ESP_LOGW(TAG, "No free slot for port %u", (unsigned)port);
    }

    if (s_task == NULL) {
        if (xTaskCreatePinnedToCoreWithCaps(proxy_task, "wled_proxy", TASK_STACK, NULL,
                                            TASK_PRIORITY, &s_task, 0, TASK_STACK_CAPS) != pdPASS) {
            ESP_LOGE(TAG, "Could not start the proxy task");
            s_task = NULL;
        }
    }
}
