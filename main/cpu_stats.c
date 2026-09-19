/**
 * @file cpu_stats.c
 * @brief Diagnostic: per-task CPU load since the previous call. See header.
 */
#include "cpu_stats.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

#define CPU_STATS_MAX_TASKS 64
#define CPU_STATS_MAX_SHOWN 12

typedef struct {
    TaskHandle_t handle;
    configRUN_TIME_COUNTER_TYPE runtime;
} prev_entry_t;

/* PSRAM: purely diagnostic, never worth internal RAM. */
static TaskStatus_t *s_now;
static prev_entry_t *s_prev;
static UBaseType_t s_prev_count;
static configRUN_TIME_COUNTER_TYPE s_prev_total;

static configRUN_TIME_COUNTER_TYPE prev_runtime(TaskHandle_t handle, bool *found)
{
    for (UBaseType_t i = 0; i < s_prev_count; ++i) {
        if (s_prev[i].handle == handle) {
            *found = true;
            return s_prev[i].runtime;
        }
    }
    *found = false;
    return 0;
}

void cpu_stats_log(const char *tag, unsigned min_percent)
{
    if (s_now == NULL) {
        s_now = heap_caps_calloc(CPU_STATS_MAX_TASKS, sizeof(*s_now),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_prev = heap_caps_calloc(CPU_STATS_MAX_TASKS, sizeof(*s_prev),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_now == NULL || s_prev == NULL) {
            return;
        }
    }

    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t count = uxTaskGetSystemState(s_now, CPU_STATS_MAX_TASKS, &total);
    if (count == 0) {
        return;
    }

    /* The run-time clock is esp_timer based, so the elapsed total is wall
     * time in µs -- i.e. exactly 100 % of one core. */
    const configRUN_TIME_COUNTER_TYPE elapsed = total - s_prev_total;
    const bool have_baseline = (s_prev_total != 0 && elapsed > 0);

    /*
     * Static, not on the stack: the server calls this from snapstats, whose
     * 3 kB stack overflowed with these two buffers on it (seen on device
     * 2026-09-19). Safe because each role has exactly one caller.
     */
    static struct {
        const char *name;
        unsigned permille;
        int core;
    } shown[CPU_STATS_MAX_SHOWN];
    static char line[400];
    size_t shown_count = 0;

    if (have_baseline) {
        for (UBaseType_t i = 0; i < count; ++i) {
            bool found = false;
            const configRUN_TIME_COUNTER_TYPE before = prev_runtime(s_now[i].xHandle, &found);
            if (!found) {
                continue; /* task born in this window, no fair baseline */
            }
            const unsigned permille =
                (unsigned)(((uint64_t)(s_now[i].ulRunTimeCounter - before) * 1000U) / elapsed);
            if (permille < min_percent * 10U) {
                continue;
            }
            /* Insertion into a small top list, busiest first. */
            size_t pos = shown_count;
            while (pos > 0 && shown[pos - 1].permille < permille) {
                --pos;
            }
            if (pos >= CPU_STATS_MAX_SHOWN) {
                continue;
            }
            const size_t last = (shown_count < CPU_STATS_MAX_SHOWN) ? shown_count : CPU_STATS_MAX_SHOWN - 1;
            memmove(&shown[pos + 1], &shown[pos], (last - pos) * sizeof(shown[0]));
            shown[pos].name = s_now[i].pcTaskName;
            shown[pos].permille = permille;
            shown[pos].core = (s_now[i].xCoreID == tskNO_AFFINITY) ? -1 : (int)s_now[i].xCoreID;
            if (shown_count < CPU_STATS_MAX_SHOWN) {
                ++shown_count;
            }
        }

        int used = snprintf(line, sizeof(line), "cpu (%% of one core, 5 s):");
        for (size_t i = 0; i < shown_count && used > 0 && (size_t)used < sizeof(line); ++i) {
            char core = (shown[i].core < 0) ? '-' : (char)('0' + shown[i].core);
            used += snprintf(line + used, sizeof(line) - (size_t)used, " %s/%c=%u.%u",
                             shown[i].name, core,
                             shown[i].permille / 10U, shown[i].permille % 10U);
        }
        ESP_LOGI(tag, "%s", line);
    }

    for (UBaseType_t i = 0; i < count; ++i) {
        s_prev[i].handle = s_now[i].xHandle;
        s_prev[i].runtime = s_now[i].ulRunTimeCounter;
    }
    s_prev_count = count;
    s_prev_total = total;
}

#else

void cpu_stats_log(const char *tag, unsigned min_percent)
{
    (void)tag;
    (void)min_percent;
}

#endif
