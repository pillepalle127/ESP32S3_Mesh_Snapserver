/**
 * @file cpu_stats.h
 * @brief Diagnostic: per-task CPU load since the previous call.
 */
#pragma once

/*
 * Logs the tasks that used at least min_percent of one core since the last
 * call, busiest first, with the core each one is pinned to ('-' = either).
 * Percent of ONE core, so a fully busy dual-core chip sums to 200 %.
 *
 * Introduced 2026-09-19 because decoding a 20 ms voice packet measured 21 ms
 * wall time on the server against ~1-3 ms in micro-opus' own benchmark --
 * wall time can't tell "slow decoder" from "decoder barely gets the core",
 * this can. Call from a low-priority stats task only, it walks every task.
 * Not reentrant: one caller per device (snapstats on the server, the
 * player's stats on a client).
 */
void cpu_stats_log(const char *tag, unsigned min_percent);
