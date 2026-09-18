/*
 * ESP32-S3 Mini Snapserver - Control Server (JSON-RPC 2.0, port 1705)
 *
 * Implements the minimum of the Snapcast control protocol required by GUI
 * controllers to connect, display the server and list the connected clients.
 * Messages are newline (CR/LF) delimited JSON-RPC 2.0 objects.
 *
 * The client list is queried from the streaming server on port 1704 via
 * snapserver_get_clients(), so controllers show the actually connected
 * players instead of an empty group.
 *
 * Supported methods:
 *   Server.GetStatus       -> full server/group/stream description
 *   Server.GetRPCVersion   -> {major,minor,patch}
 *   Client.GetStatus       -> single client object
 *   Client.SetVolume       -> applies and echoes the requested volume
 *   Client.SetName         -> applies and echoes the requested name
 *   Client.SetLatency      -> applies and echoes the requested latency
 *   Group.GetStatus        -> the single group object
 *   Group.SetStream        -> echoes the requested stream_id
 *   Group.SetMute          -> echoes the requested mute flag
 *   Group.SetClients       -> returns the current server object
 *   Server.DeleteClient    -> returns the current server object
 * Any other method returns a JSON-RPC "method not found" error.
 *
 * The stream is advertised as codec "opus", sampleformat 48000:16:1 (mono),
 * matching the encoder used on the 1704 audio path.
 */
#include "snapcontrol.h"
#include "snapserver.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SNAPCONTROL";

#define CTRL_MAX_CONN     4
#define CTRL_RX_MAX       2048
/*
 * Three functions here keep a snapserver_client_info_t[SNAPSERVER_MAX_CLIENTS]
 * on the stack, and that struct is ~344 B. Raising the client limit to 10
 * grew each of those arrays to ~3.4 KB, which together with the cJSON
 * response build put the connection task at its limit -- a stack overflow
 * there looks like "the control app lists no clients" rather than like a
 * crash. Sized with headroom for the full client count.
 */
/*
 * Back down from 12288: the three snapshot helpers below used to put a
 * snapserver_client_info_t[SNAPSERVER_MAX_CLIENTS] on the stack, about
 * 3.3 kB each at 10 clients, and those arrays now come from PSRAM instead.
 * Internal DRAM is the scarce resource on this device -- the Wi-Fi driver
 * allocates its TX buffers from it, and a control connection must not cost
 * 12 kB of it.
 */
#define CTRL_CONN_STACK   6144
#define CTRL_SERVER_STACK 6144
#define CTRL_REFRESH_MS    500

#define SERVER_NAME       "esp32-s3-mini-snapserver"
#define SERVER_VERSION    "0.27.0"
#define STREAM_ID         "default"
#define GROUP_ID          "esp32-mini-group"
#define SAMPLE_FORMAT     "48000:16:1"

static TaskHandle_t s_server_task;
static bool s_started;

static int send_all(int fd, const void *data, size_t length)
{
    const uint8_t *cursor = (const uint8_t *)data;

    while (length > 0U) {
        const int sent = send(fd, cursor, length, 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            ESP_LOGW(TAG,
                     "Control send failed: fd=%d errno=%d (%s)",
                     fd,
                     errno,
                     strerror(errno));
            return -1;
        }
        cursor += sent;
        length -= (size_t)sent;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void get_mac_str(char *out, size_t len)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void get_ip_str(char *out, size_t len)
{
    esp_netif_t *netif =
        esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    esp_netif_ip_info_t ip = {0};

    if (netif != NULL &&
        esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
        ip.ip.addr != 0U) {
        snprintf(out, len, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(out, len, "192.168.5.1");
    }
}

/*
 * Builds one client object in the layout expected by Snapcast controllers.
 */
static cJSON *build_client_object(const snapserver_client_info_t *info)
{
    cJSON *client = cJSON_CreateObject();
    if (client == NULL) {
        return NULL;
    }

    cJSON_AddBoolToObject(client, "connected", info->connected);
    cJSON_AddStringToObject(client, "id", info->id);

    cJSON *config = cJSON_AddObjectToObject(client, "config");
    cJSON_AddNumberToObject(config, "instance", info->instance);
    cJSON_AddNumberToObject(config, "latency", info->latency_ms);
    cJSON_AddStringToObject(config, "name", info->name);

    cJSON *volume = cJSON_AddObjectToObject(config, "volume");
    cJSON_AddBoolToObject(volume, "muted", info->muted);
    cJSON_AddNumberToObject(volume, "percent", info->volume_percent);

    cJSON *host = cJSON_AddObjectToObject(client, "host");
    cJSON_AddStringToObject(host, "arch", info->arch);
    cJSON_AddStringToObject(host, "ip", info->ip);
    cJSON_AddStringToObject(host, "mac", info->mac);
    cJSON_AddStringToObject(host, "name", info->hostname);
    cJSON_AddStringToObject(host, "os", info->os);

    cJSON *last_seen = cJSON_AddObjectToObject(client, "lastSeen");
    cJSON_AddNumberToObject(last_seen, "sec", info->last_seen_sec);
    cJSON_AddNumberToObject(last_seen, "usec", info->last_seen_usec);

    cJSON *snapclient = cJSON_AddObjectToObject(client, "snapclient");
    cJSON_AddStringToObject(snapclient, "name", "Snapclient");
    cJSON_AddNumberToObject(snapclient, "protocolVersion",
                            info->protocol_ver);
    cJSON_AddStringToObject(snapclient, "version", info->version);

    return client;
}

/*
 * Snapshot buffer for the helpers below. PSRAM rather than the stack: at
 * SNAPSERVER_MAX_CLIENTS = 10 the array is ~3.3 kB, which is a lot to
 * reserve in every control-connection task, and internal DRAM is what the
 * Wi-Fi driver competes for. Returns 0 on failure, which the callers treat
 * as "no clients" -- a control reply is worth less than staying up.
 */
static size_t take_client_snapshot(snapserver_client_info_t **out)
{
    snapserver_client_info_t *buf =
        heap_caps_malloc(sizeof(*buf) * SNAPSERVER_MAX_CLIENTS, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = heap_caps_malloc(sizeof(*buf) * SNAPSERVER_MAX_CLIENTS,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        *out = NULL;
        return 0;
    }

    *out = buf;
    return snapserver_get_clients(buf, SNAPSERVER_MAX_CLIENTS);
}

/* Appends every connected client to the given array. */
static void fill_client_array(cJSON *array)
{
    snapserver_client_info_t *clients = NULL;
    const size_t count = take_client_snapshot(&clients);

    for (size_t i = 0; i < count; ++i) {
        cJSON *entry = build_client_object(&clients[i]);
        if (entry != NULL) {
            cJSON_AddItemToArray(array, entry);
        }
    }

    free(clients);
}

/* Builds the single group object including its clients. */
static cJSON *build_group_object(void)
{
    cJSON *group = cJSON_CreateObject();
    if (group == NULL) {
        return NULL;
    }

    cJSON *clients = cJSON_AddArrayToObject(group, "clients");
    fill_client_array(clients);

    cJSON_AddStringToObject(group, "id", GROUP_ID);
    cJSON_AddBoolToObject(group, "muted", false);
    cJSON_AddStringToObject(group, "name", "");
    cJSON_AddStringToObject(group, "stream_id", STREAM_ID);

    return group;
}

/* Builds the "server" object used by Server.GetStatus. */
static cJSON *build_server_object(void)
{
    char mac[18];
    char ip[16];

    get_mac_str(mac, sizeof(mac));
    get_ip_str(ip, sizeof(ip));

    cJSON *server = cJSON_CreateObject();
    if (server == NULL) {
        return NULL;
    }

    /* groups: one group holding all connected clients */
    cJSON *groups = cJSON_AddArrayToObject(server, "groups");
    cJSON *group = build_group_object();
    if (group != NULL) {
        cJSON_AddItemToArray(groups, group);
    }

    /* server.server.host + server.server.snapserver */
    cJSON *srv = cJSON_AddObjectToObject(server, "server");
    cJSON *host = cJSON_AddObjectToObject(srv, "host");
    cJSON_AddStringToObject(host, "arch", "xtensa");
    cJSON_AddStringToObject(host, "ip", ip);
    cJSON_AddStringToObject(host, "mac", mac);
    cJSON_AddStringToObject(host, "name", SERVER_NAME);
    cJSON_AddStringToObject(host, "os", "esp-idf");

    cJSON *snapserver = cJSON_AddObjectToObject(srv, "snapserver");
    cJSON_AddNumberToObject(snapserver, "controlProtocolVersion", 1);
    cJSON_AddStringToObject(snapserver, "name", "Snapserver");
    cJSON_AddNumberToObject(snapserver, "protocolVersion", 1);
    cJSON_AddStringToObject(snapserver, "version", SERVER_VERSION);

    /* streams: one opus stream, playing */
    cJSON *streams = cJSON_AddArrayToObject(server, "streams");
    cJSON *stream = cJSON_CreateObject();
    cJSON_AddItemToArray(streams, stream);
    cJSON_AddStringToObject(stream, "id", STREAM_ID);
    cJSON_AddStringToObject(stream, "status", "playing");

    cJSON *uri = cJSON_AddObjectToObject(stream, "uri");
    cJSON_AddStringToObject(uri, "fragment", "");
    cJSON_AddStringToObject(uri, "host", "");
    cJSON_AddStringToObject(uri, "path", "/tmp/snapfifo");
    cJSON *query = cJSON_AddObjectToObject(uri, "query");
    cJSON_AddStringToObject(query, "codec", "opus");
    cJSON_AddStringToObject(query, "name", STREAM_ID);
    cJSON_AddStringToObject(query, "sampleformat", SAMPLE_FORMAT);
    cJSON_AddStringToObject(uri, "raw",
                            "pipe:///tmp/snapfifo?name=" STREAM_ID
                            "&codec=opus&sampleformat=" SAMPLE_FORMAT);
    cJSON_AddStringToObject(uri, "scheme", "pipe");

    return server;
}

/* Returns the "id" parameter as string, or NULL. */
static const char *param_id(const cJSON *params)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "id");
    return cJSON_IsString(id) ? id->valuestring : NULL;
}

/* Looks up one client snapshot by id. Returns false if unknown. */
static bool lookup_client(const char *id, snapserver_client_info_t *out)
{
    if (id == NULL || out == NULL) {
        return false;
    }

    snapserver_client_info_t *clients = NULL;
    const size_t count = take_client_snapshot(&clients);

    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(clients[i].id, id) == 0) {
            *out = clients[i];
            found = true;
            break;
        }
    }

    free(clients);
    return found;
}

/* Copies the "volume" object from params, or a sane default. */
static cJSON *dup_volume_or_default(const cJSON *params)
{
    const cJSON *vol = cJSON_GetObjectItemCaseSensitive(params, "volume");
    if (cJSON_IsObject(vol)) {
        return cJSON_Duplicate(vol, true);
    }

    cJSON *fallback = cJSON_CreateObject();
    cJSON_AddBoolToObject(fallback, "muted", false);
    cJSON_AddNumberToObject(fallback, "percent", 100);
    return fallback;
}

/* Builds the JSON-RPC result object for a given method. */
static cJSON *build_result(const char *method, const cJSON *params)
{
    if (strcmp(method, "Server.GetStatus") == 0) {
        /*
         * Snapcast control schema:
         *   result.server.groups[]
         *   result.server.server
         *   result.server.streams[]
         *
         * The previous implementation returned groups/server/streams directly
         * below result. Snapdroid therefore connected successfully but found no
         * clients at result.server.groups[].clients[].
         */
        cJSON *status = cJSON_CreateObject();
        if (status == NULL) {
            return NULL;
        }

        cJSON *server = build_server_object();
        if (server == NULL) {
            cJSON_Delete(status);
            return NULL;
        }

        cJSON_AddItemToObject(status, "server", server);
        return status;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return NULL;
    }

    if (strcmp(method, "Server.DeleteClient") == 0 ||
        strcmp(method, "Group.SetClients") == 0) {
        cJSON_AddItemToObject(result, "server", build_server_object());

    } else if (strcmp(method, "Server.GetRPCVersion") == 0) {
        cJSON_AddNumberToObject(result, "major", 2);
        cJSON_AddNumberToObject(result, "minor", 0);
        cJSON_AddNumberToObject(result, "patch", 0);

    } else if (strcmp(method, "Group.GetStatus") == 0) {
        cJSON *group = build_group_object();
        if (group == NULL) {
            cJSON_Delete(result);
            return NULL;
        }
        cJSON_AddItemToObject(result, "group", group);

    } else if (strcmp(method, "Client.GetStatus") == 0) {
        snapserver_client_info_t info;
        if (!lookup_client(param_id(params), &info)) {
            cJSON_Delete(result);
            return NULL;
        }
        cJSON_AddItemToObject(result, "client", build_client_object(&info));

    } else if (strcmp(method, "Client.SetVolume") == 0) {
        const cJSON *vol =
            cJSON_GetObjectItemCaseSensitive(params, "volume");
        const cJSON *percent =
            cJSON_GetObjectItemCaseSensitive(vol, "percent");
        const cJSON *muted =
            cJSON_GetObjectItemCaseSensitive(vol, "muted");

        const char *id = param_id(params);
        if (id != NULL) {
            snapserver_set_client_volume(
                id,
                cJSON_IsNumber(percent) ? (int32_t)percent->valuedouble : 100,
                cJSON_IsTrue(muted));
        }

        cJSON_AddItemToObject(
            result,
            "volume",
            dup_volume_or_default(params));

    } else if (strcmp(method, "Client.SetName") == 0) {
        const cJSON *name =
            cJSON_GetObjectItemCaseSensitive(params, "name");
        const char *name_str = cJSON_IsString(name) ? name->valuestring : "";
        const char *id = param_id(params);

        if (id != NULL) {
            snapserver_set_client_name(id, name_str);
        }

        cJSON_AddStringToObject(result, "name", name_str);

    } else if (strcmp(method, "Client.SetLatency") == 0) {
        const cJSON *latency =
            cJSON_GetObjectItemCaseSensitive(params, "latency");
        const int32_t latency_ms =
            cJSON_IsNumber(latency) ? (int32_t)latency->valuedouble : 0;
        const char *id = param_id(params);

        if (id != NULL) {
            snapserver_set_client_latency(id, latency_ms);
        }

        cJSON_AddNumberToObject(result, "latency", latency_ms);

    } else if (strcmp(method, "Group.SetStream") == 0) {
        const cJSON *stream_id =
            cJSON_GetObjectItemCaseSensitive(params, "stream_id");

        cJSON_AddStringToObject(
            result,
            "stream_id",
            cJSON_IsString(stream_id)
                ? stream_id->valuestring
                : STREAM_ID);

    } else if (strcmp(method, "Group.SetMute") == 0) {
        const cJSON *mute =
            cJSON_GetObjectItemCaseSensitive(params, "mute");

        cJSON_AddBoolToObject(
            result,
            "mute",
            cJSON_IsTrue(mute));

    } else {
        cJSON_Delete(result);
        return NULL;
    }

    return result;
}

/* Serializes a JSON-RPC response for one request object. Returns malloc'ed
 * string (caller frees) or NULL on parse/alloc failure. */
static char *handle_request_object(const cJSON *req)
{
    const cJSON *id =
        cJSON_GetObjectItemCaseSensitive(req, "id");

    const cJSON *method =
        cJSON_GetObjectItemCaseSensitive(req, "method");

    const cJSON *params =
        cJSON_GetObjectItemCaseSensitive(req, "params");

    if (cJSON_IsString(method)) {
        ESP_LOGI(TAG, "RPC request: %s", method->valuestring);
    } else {
        ESP_LOGW(TAG, "RPC request without valid method");
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        ESP_LOGE(TAG, "Could not allocate JSON-RPC response");
        return NULL;
    }

    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(
        resp,
        "id",
        id != NULL
            ? cJSON_Duplicate(id, true)
            : cJSON_CreateNull());

    if (!cJSON_IsString(method)) {
        cJSON *error = cJSON_AddObjectToObject(resp, "error");
        cJSON_AddNumberToObject(error, "code", -32600);
        cJSON_AddStringToObject(
            error,
            "message",
            "Invalid Request");
    } else {
        cJSON *result =
            build_result(method->valuestring, params);

        if (result != NULL) {
            cJSON_AddItemToObject(resp, "result", result);
        } else {
            cJSON *error =
                cJSON_AddObjectToObject(resp, "error");

            cJSON_AddNumberToObject(error, "code", -32601);
            cJSON_AddStringToObject(
                error,
                "message",
                "Method not found");
        }
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    return out;
}

/* Parses one line (single object or batch array) and sends the reply. */
static void process_line(int fd, const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW(TAG, "Ignoring malformed JSON");
        return;
    }

    if (cJSON_IsArray(root)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, root) {
            char *resp = handle_request_object(item);
            if (resp != NULL) {
                ESP_LOGI(TAG, "RPC response: %s", resp);
                if (send_all(fd, resp, strlen(resp)) != 0 ||
                    send_all(fd, "\r\n", 2U) != 0) {
                    ESP_LOGW(TAG, "Control response could not be sent completely");
                }
                free(resp);
            }
        }
    } else {
        char *resp = handle_request_object(root);
        if (resp != NULL) {
            ESP_LOGI(TAG, "RPC response: %s", resp);
            if (send_all(fd, resp, strlen(resp)) != 0 ||
                send_all(fd, "\r\n", 2U) != 0) {
                ESP_LOGW(TAG, "Control response could not be sent completely");
            }
            free(resp);
        }
    }

    cJSON_Delete(root);
}

/* Builds and sends a complete Server.OnUpdate notification. */
static int send_server_update(int fd)
{
    cJSON *notification = cJSON_CreateObject();
    if (notification == NULL) {
        return -1;
    }

    cJSON_AddStringToObject(notification, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notification, "method", "Server.OnUpdate");

    cJSON *params = cJSON_AddObjectToObject(notification, "params");
    cJSON *server = build_server_object();
    if (params == NULL || server == NULL) {
        cJSON_Delete(server);
        cJSON_Delete(notification);
        return -1;
    }
    cJSON_AddItemToObject(params, "server", server);

    char *text = cJSON_PrintUnformatted(notification);
    cJSON_Delete(notification);
    if (text == NULL) {
        return -1;
    }

    const int rc =
        (send_all(fd, text, strlen(text)) == 0 &&
         send_all(fd, "\r\n", 2U) == 0) ? 0 : -1;
    free(text);
    return rc;
}

/* Stable fingerprint of the current client set for change detection. */
static uint32_t client_set_fingerprint(void)
{
    return snapserver_client_set_hash();
}

/* ------------------------------------------------------------------ */
/* Tasks                                                              */
/* ------------------------------------------------------------------ */

static void ctrl_conn_task(void *arg)
{
    const int fd = (int)(intptr_t)arg;
    char *buf = malloc(CTRL_RX_MAX);

    if (buf == NULL) {
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    const struct timeval timeout = {
        .tv_sec = CTRL_REFRESH_MS / 1000,
        .tv_usec = (CTRL_REFRESH_MS % 1000) * 1000,
    };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout));

    size_t used = 0;
    uint32_t last_fingerprint = UINT32_MAX;

    for (;;) {
        const int n = recv(fd, buf + used, CTRL_RX_MAX - 1U - used, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const uint32_t current = client_set_fingerprint();
            if (current != last_fingerprint) {
                if (send_server_update(fd) != 0) {
                    break;
                }
                last_fingerprint = current;
                ESP_LOGI(TAG, "Server.OnUpdate sent after client-set change");
            }
            continue;
        }

        if (n <= 0) {
            break;
        }

        used += (size_t)n;
        buf[used] = '\0';

        char *start = buf;
        char *nl;
        while ((nl = memchr(start, '\n', (size_t)(buf + used - start))) != NULL) {
            *nl = '\0';
            size_t len = strlen(start);
            if (len > 0U && start[len - 1U] == '\r') {
                start[len - 1U] = '\0';
            }
            if (start[0] != '\0') {
                process_line(fd, start);
                last_fingerprint = client_set_fingerprint();
            }
            start = nl + 1;
        }

        const size_t rem = (size_t)(buf + used - start);
        memmove(buf, start, rem);
        used = rem;

        if (used >= CTRL_RX_MAX - 1U) {
            ESP_LOGW(TAG, "RX buffer overflow, dropping data");
            used = 0;
        }
    }

    free(buf);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    ESP_LOGI(TAG, "Control client disconnected");
    vTaskDelete(NULL);
}

static void ctrl_server_task(void *arg)
{
    (void)arg;

    const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        s_server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SNAPCONTROL_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed errno=%d", errno);
        close(listen_fd);
        s_server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, CTRL_MAX_CONN) < 0) {
        ESP_LOGE(TAG, "listen failed errno=%d", errno);
        close(listen_fd);
        s_server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "JSON-RPC control server listening on TCP port %d",
             SNAPCONTROL_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int fd = accept(listen_fd,
                              (struct sockaddr *)&peer,
                              &peer_len);
        if (fd < 0) {
            if (errno != EINTR) {
                ESP_LOGW(TAG, "accept failed errno=%d", errno);
            }
            continue;
        }

        ESP_LOGI(TAG, "Control client connected");
        if (xTaskCreatePinnedToCore(ctrl_conn_task,
                                    "snapctrl_conn",
                                    CTRL_CONN_STACK,
                                    (void *)(intptr_t)fd,
                                    4,
                                    NULL,
                                    0) != pdPASS) {
            ESP_LOGE(TAG, "Could not create control connection task");
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t snapcontrol_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    if (xTaskCreatePinnedToCore(ctrl_server_task,
                                "snapcontrol",
                                CTRL_SERVER_STACK,
                                NULL,
                                5,
                                &s_server_task,
                                0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}
