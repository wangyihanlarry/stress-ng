/*
 * Copyright (C) 2025 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#ifndef CORE_PRECISE_MEMORY_H
#define CORE_PRECISE_MEMORY_H

#include "stress-ng.h"

/* System memory information structure */
typedef struct {
    uint64_t total_memory;        /* 系统总内存（字节） */
    uint64_t used_memory;         /* 当前已使用内存 */
    uint64_t available_memory;    /* 可用内存 */
    double usage_percentage;      /* 当前使用百分比 */
    uint64_t timestamp;           /* 时间戳（微秒） */
    
    /* Platform-specific fields */
    bool is_macos;                /* 是否为 macOS */
    bool is_apple_silicon;        /* 是否为 Apple Silicon */
    
    /* macOS specific memory info */
    uint64_t wired_memory;        /* 联线内存 */
    uint64_t active_memory;       /* 活跃内存 */
    uint64_t inactive_memory;     /* 非活跃内存 */
    uint64_t compressed_memory;   /* 压缩内存 */
    uint64_t free_memory;         /* 空闲内存 */
} system_memory_info_t;

/* Function declarations */
extern int get_system_memory_info(system_memory_info_t *info);
extern uint64_t get_total_physical_memory(void);
extern double calculate_memory_usage_percent(void);

/* Platform-specific implementations */
#ifdef __APPLE__
extern int get_macos_memory_info(system_memory_info_t *info);
extern uint64_t get_macos_total_memory(void);
extern bool is_apple_silicon_mac(void);
#endif

#ifdef __linux__
extern int get_linux_memory_info(system_memory_info_t *info);
extern uint64_t get_linux_total_memory(void);
#endif

/* Utility functions */
extern uint64_t get_current_timestamp_us(void);
extern void clear_memory_info(system_memory_info_t *info);

/* Real-time calculation functions */
extern double calculate_memory_usage_percent_realtime(void);
extern int get_memory_usage_with_timestamp(double *usage_percent, uint64_t *timestamp);
extern void invalidate_memory_cache(void);

/* Adaptive memory allocator structures */
typedef struct memory_chunk {
    void *ptr;                    /* 内存块指针 */
    size_t size;                  /* 内存块大小 */
    struct memory_chunk *next;    /* 链表下一个节点 */
} memory_chunk_t;

typedef struct {
    memory_chunk_t *chunks;       /* 已分配内存块链表 */
    uint64_t total_allocated;     /* 总分配内存量 */
    size_t chunk_size;            /* 单个内存块大小（通常为页面大小的倍数） */
    pthread_mutex_t mutex;        /* 线程安全锁 */
    bool should_stop;             /* 停止标志 */
} adaptive_allocator_t;

/* Adaptive allocator functions */
extern int adaptive_allocator_init(adaptive_allocator_t *allocator);
extern int adaptive_allocator_adjust_to_target(adaptive_allocator_t *allocator, uint64_t target_bytes);
extern uint64_t adaptive_allocator_get_allocated(adaptive_allocator_t *allocator);
extern void adaptive_allocator_cleanup(adaptive_allocator_t *allocator);

/* Stabilization and control structures */
typedef struct {
    uint32_t stabilization_window;   /* 稳定期判定窗口（采样次数，默认10） */
    double stabilization_threshold;  /* 稳定期判定阈值（默认2%，比目标阈值更严格） */
    uint32_t min_stable_duration;    /* 最小稳定持续时间（秒，默认5秒） */
    uint32_t monitor_interval_ms;    /* 监控间隔（毫秒，默认1000） */
    uint32_t adjustment_interval_ms; /* 调整间隔（毫秒，默认500） */
    uint32_t max_stabilization_time; /* 最大稳定化时间（秒，默认60秒） */
} stabilization_config_t;

typedef struct {
    double target_percentage;        /* 目标百分比 */
    uint64_t target_bytes;           /* 目标字节数 */
    uint64_t duration_seconds;       /* 稳定期持续时间（不包括达到稳定的时间） */
    uint64_t start_time;             /* 开始时间（微秒） */
    uint64_t stabilization_time;     /* 达到稳定期的时间（微秒） */
    uint64_t stable_period_start;    /* 稳定期开始时间（微秒） */
    uint64_t stable_period_end;      /* 稳定期结束时间（微秒） */
    uint64_t end_time;               /* 总结束时间（微秒） */
    bool is_stabilized;              /* 是否已达到稳定期 */
    uint32_t stable_sample_count;    /* 稳定期内的采样次数 */
    uint32_t consecutive_stable_samples; /* 连续稳定采样次数 */
    uint32_t required_stable_samples;    /* 判定稳定所需的连续采样次数 */
    uint32_t total_sample_count;     /* 总采样次数 */
    double sum_usage_percentage;     /* 使用率总和（用于计算平均值） */
    double stable_period_avg_usage;  /* 稳定期平均使用率 */
    double max_deviation;            /* 最大偏差 */
    double min_deviation;            /* 最小偏差 */
    uint32_t adjustment_count;       /* 调整次数 */
    uint64_t total_allocated;        /* 总分配内存 */
    uint64_t peak_allocated;         /* 峰值分配内存 */
    uint32_t allocation_failures;    /* 分配失败次数 */
    uint32_t within_threshold_count; /* 在阈值内的采样次数 */
    uint32_t stable_within_threshold_count; /* 稳定期内在阈值内的采样次数 */
    double overall_accuracy_percentage;      /* 总体精度百分比 */
    double stable_period_accuracy_percentage; /* 稳定期精度百分比 */
} precise_memory_stats_t;

typedef struct {
    double target_percentage;     /* 目标百分比 */
    uint64_t duration_seconds;    /* 稳定期持续时间（不包括达到稳定的时间） */
    uint64_t total_memory;        /* 系统总内存 */
    uint64_t target_bytes;        /* 目标内存使用量 */
    double accuracy_threshold;    /* 精度阈值 */
    
    adaptive_allocator_t allocator;
    system_memory_info_t current_info;
    stabilization_config_t stab_config;
    
    /* 稳定期状态 */
    bool is_stabilized;           /* 是否已达到稳定期 */
    uint64_t stabilization_start_time; /* 开始稳定化的时间 */
    uint64_t stable_period_start_time; /* 稳定期开始时间 */
    double recent_usage_samples[10];   /* 最近的使用率采样（用于稳定期判定） */
    uint32_t sample_index;        /* 当前采样索引 */
    
    /* 统计信息 */
    precise_memory_stats_t stats;
} precise_control_context_t;

/* Control and stabilization functions */
extern int precise_control_context_init(precise_control_context_t *ctx, 
                                       double target_percentage, 
                                       uint64_t duration_seconds,
                                       double accuracy_threshold);
extern void precise_control_context_cleanup(precise_control_context_t *ctx);
extern bool check_stabilization(precise_control_context_t *ctx);
extern int adjust_memory_allocation(precise_control_context_t *ctx);
extern void log_phase_transition(const precise_control_context_t *ctx, const char *phase);
extern void log_final_statistics(precise_control_context_t *ctx);

#endif /* CORE_PRECISE_MEMORY_H */