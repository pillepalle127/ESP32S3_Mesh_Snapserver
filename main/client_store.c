/**
 * @file client_store.c
 * @brief Per-client settings in NVS, served from a RAM copy.
 */
#include "client_store.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "CLIENT_STORE";

#define STORE_NVS_NAMESPACE "clients"
#define STORE_VERSION       1U

/*
 * One NVS blob per client. The key is a hash of the id, since an id can be
 * longer than the 15 characters a key may have; the id itself is stored
 * too, so a hash collision reads as "not found" rather than as some other
 * speaker's volume.
 */
typedef struct {
    uint8_t version;
    uint8_t muted;
    int16_t latency_ms;
    int32_t volume_percent;
    uint32_t seq; /* write order, to find the entry to drop when full */
    char id[64];
    char name[64];
} stored_t;

/* PSRAM, ~3.4 kB. Guarded by s_lock; id[0] == '\0' marks a free slot. */
static stored_t *s_entries;
static uint32_t s_seq;
static SemaphoreHandle_t s_lock;

static void make_key(const char *id, char key[NVS_KEY_NAME_MAX_SIZE])
{
    uint32_t hash = 2166136261U;
    for (const unsigned char *p = (const unsigned char *)id; *p != '\0'; ++p) {
        hash ^= *p;
        hash *= 16777619U;
    }
    snprintf(key, NVS_KEY_NAME_MAX_SIZE, "c%08" PRIx32, hash);
}

static stored_t *find_unsafe(const char *id)
{
    for (size_t i = 0; i < CLIENT_STORE_MAX; ++i) {
        if (s_entries[i].id[0] != '\0' && strcmp(s_entries[i].id, id) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

esp_err_t client_store_init(void)
{
    if (s_entries != NULL) {
        return ESP_OK;
    }

    s_entries = heap_caps_calloc(CLIENT_STORE_MAX, sizeof(*s_entries), MALLOC_CAP_SPIRAM);
    if (s_entries == NULL) {
        s_entries = calloc(CLIENT_STORE_MAX, sizeof(*s_entries));
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_entries == NULL || s_lock == NULL) {
        ESP_LOGE(TAG, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* nothing stored yet */
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(result));
        return ESP_OK; /* start empty rather than without a server */
    }

    size_t loaded = 0;
    nvs_iterator_t it = NULL;
    esp_err_t found = nvs_entry_find(NVS_DEFAULT_PART_NAME, STORE_NVS_NAMESPACE,
                                     NVS_TYPE_BLOB, &it);
    while (found == ESP_OK && loaded < CLIENT_STORE_MAX) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        stored_t entry;
        size_t len = sizeof(entry);
        if (nvs_get_blob(handle, info.key, &entry, &len) == ESP_OK &&
            len == sizeof(entry) && entry.version == STORE_VERSION &&
            memchr(entry.id, '\0', sizeof(entry.id)) != NULL && entry.id[0] != '\0' &&
            memchr(entry.name, '\0', sizeof(entry.name)) != NULL) {
            s_entries[loaded++] = entry;
            if (entry.seq > s_seq) {
                s_seq = entry.seq;
            }
        }
        found = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(handle);

    ESP_LOGI(TAG, "%u stored client setting(s) loaded", (unsigned)loaded);
    return ESP_OK;
}

bool client_store_get(const char *id, client_store_entry_t *out)
{
    if (s_entries == NULL || id == NULL || id[0] == '\0') {
        return false;
    }

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const stored_t *entry = find_unsafe(id);
    if (entry != NULL) {
        out->volume_percent = entry->volume_percent;
        out->muted = entry->muted != 0U;
        out->latency_ms = entry->latency_ms;
        strlcpy(out->name, entry->name, sizeof(out->name));
        found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
}

void client_store_put(const char *id, const client_store_entry_t *entry)
{
    if (s_entries == NULL || id == NULL || id[0] == '\0' ||
        strlen(id) >= sizeof(((stored_t *)0)->id)) {
        return;
    }

    stored_t next = {
        .version = STORE_VERSION,
        .muted = entry->muted ? 1U : 0U,
        .latency_ms = (int16_t)((entry->latency_ms > INT16_MAX) ? INT16_MAX
                              : (entry->latency_ms < INT16_MIN) ? INT16_MIN
                              : entry->latency_ms),
        .volume_percent = entry->volume_percent,
    };
    strlcpy(next.id, id, sizeof(next.id));
    strlcpy(next.name, entry->name, sizeof(next.name));

    char evicted_key[NVS_KEY_NAME_MAX_SIZE] = "";

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stored_t *slot = find_unsafe(id);
    if (slot != NULL && slot->muted == next.muted && slot->latency_ms == next.latency_ms &&
        slot->volume_percent == next.volume_percent && strcmp(slot->name, next.name) == 0) {
        xSemaphoreGive(s_lock);
        return; /* unchanged, spare the flash */
    }
    if (slot == NULL) {
        for (size_t i = 0; i < CLIENT_STORE_MAX && slot == NULL; ++i) {
            if (s_entries[i].id[0] == '\0') {
                slot = &s_entries[i];
            }
        }
    }
    if (slot == NULL) {
        slot = &s_entries[0];
        for (size_t i = 1; i < CLIENT_STORE_MAX; ++i) {
            if (s_entries[i].seq < slot->seq) {
                slot = &s_entries[i];
            }
        }
        make_key(slot->id, evicted_key);
    }
    next.seq = ++s_seq;
    *slot = next;
    xSemaphoreGive(s_lock);

    char key[NVS_KEY_NAME_MAX_SIZE];
    make_key(id, key);

    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(result));
        return;
    }
    if (evicted_key[0] != '\0' && strcmp(evicted_key, key) != 0) {
        (void)nvs_erase_key(handle, evicted_key);
    }
    result = nvs_set_blob(handle, key, &next, sizeof(next));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Storing settings for %s failed: %s", id, esp_err_to_name(result));
    }
}

void client_store_erase_all(void)
{
    nvs_handle_t handle;
    if (nvs_open(STORE_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_erase_all(handle) == ESP_OK) {
            (void)nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (s_entries != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memset(s_entries, 0, sizeof(*s_entries) * CLIENT_STORE_MAX);
        xSemaphoreGive(s_lock);
    }
}
