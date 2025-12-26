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
#include "stress-ng.h"
#include "core-precise-memory.h"
#include "core-builtin.h"

#include <sys/time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <math.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>
#endif

#if defined(__linux__)
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#endif

/* Cache for memory information to improve performance */
static system_memory_info_t cached_info;
static uint64_t cache_timestamp = 0;
static const uint64_t CACHE_VALIDITY_US = 100000; /* 100ms cache validity */

/*
 * get_current_timestamp_us()
 *     Get current timestamp in microseconds
 */
uint64_t get_current_timestamp_us(void)
{
    struct timeval tv;
    
    if (gettimeofday(&tv, NULL) != 0)
        return 0;
    
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/*
 * clear_memory_info()
 *     Initialize memory info structure to zero
 */
void clear_memory_info(system_memory_info_t *info)
{
    if (!info)
        return;
    
    (void)shim_memset(info, 0, sizeof(*info));
}

#ifdef __APPLE__
/*
 * is_apple_silicon_mac()
 *     Detect if running on Apple Silicon (ARM64) Mac
 */
bool is_apple_silicon_mac(void)
{
    size_t size = 0;
    int ret;
    
    /* Check if we can get the CPU brand string */
    ret = sysctlbyname("machdep.cpu.brand_string", NULL, &size, NULL, 0);
    if (ret == 0 && size > 0) {
        char *cpu_brand = malloc(size);
        if (cpu_brand) {
            ret = sysctlbyname("machdep.cpu.brand_string", cpu_brand, &size, NULL, 0);
            if (ret == 0) {
                /* Check for Apple Silicon indicators */
                bool is_apple_silicon = (strstr(cpu_brand, "Apple") != NULL) ||
                                       (strstr(cpu_brand, "M1") != NULL) ||
                                       (strstr(cpu_brand, "M2") != NULL) ||
                                       (strstr(cpu_brand, "M3") != NULL);
                free(cpu_brand);
                return is_apple_silicon;
            }
            free(cpu_brand);
        }
    }
    
    /* Fallback: check architecture */
#if defined(__aarch64__) || defined(__arm64__)
    return true;
#else
    return false;
#endif
}

/*
 * get_macos_total_memory()
 *     Get total physical memory on macOS using sysctl
 */
uint64_t get_macos_total_memory(void)
{
    uint64_t total_memory = 0;
    size_t size = sizeof(total_memory);
    
    if (sysctlbyname("hw.memsize", &total_memory, &size, NULL, 0) == 0) {
        return total_memory;
    }
    
    return 0;
}

/*
 * get_macos_memory_info()
 *     Get detailed memory information on macOS
 */
int get_macos_memory_info(system_memory_info_t *info)
{
    vm_statistics64_data_t vm_stat;
    mach_port_t host_port;
    natural_t count;
    kern_return_t ret;
    uint64_t page_size;
    size_t size;
    
    if (!info)
        return -1;
    
    clear_memory_info(info);
    
    /* Get page size */
    size = sizeof(page_size);
    if (sysctlbyname("hw.pagesize", &page_size, &size, NULL, 0) != 0) {
        page_size = 4096; /* Default page size */
    }
    
    /* Get total memory */
    info->total_memory = get_macos_total_memory();
    if (info->total_memory == 0)
        return -1;
    
    /* Get VM statistics */
    host_port = mach_host_self();
    count = HOST_VM_INFO64_COUNT;
    (void)shim_memset(&vm_stat, 0, sizeof(vm_stat));
    
    ret = host_statistics64(host_port, HOST_VM_INFO64, 
                           (host_info64_t)&vm_stat, &count);
    if (ret != KERN_SUCCESS)
        return -1;
    
    /* Calculate memory values */
    info->free_memory = vm_stat.free_count * page_size;
    info->active_memory = vm_stat.active_count * page_size;
    info->inactive_memory = vm_stat.inactive_count * page_size;
    info->wired_memory = vm_stat.wire_count * page_size;
    info->compressed_memory = vm_stat.compressor_page_count * page_size;
    
    /* Calculate used memory (active + inactive + wired + compressed) */
    info->used_memory = info->active_memory + info->inactive_memory + 
                       info->wired_memory + info->compressed_memory;
    
    /* Available memory is free + inactive (can be reclaimed) */
    info->available_memory = info->free_memory + info->inactive_memory;
    
    /* Calculate usage percentage based on total physical memory */
    if (info->total_memory > 0) {
        info->usage_percentage = ((double)info->used_memory / (double)info->total_memory) * 100.0;
    }
    
    /* Set platform flags */
    info->is_macos = true;
    info->is_apple_silicon = is_apple_silicon_mac();
    info->timestamp = get_current_timestamp_us();
    
    return 0;
}
#endif /* __APPLE__ */

#ifdef __linux__
/*
 * get_linux_total_memory()
 *     Get total physical memory on Linux using /proc/meminfo
 */
uint64_t get_linux_total_memory(void)
{
    FILE *fp;
    char line[256];
    uint64_t total_memory = 0;
    
    fp = fopen("/proc/meminfo", "r");
    if (!fp)
        return 0;
    
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            unsigned long kb;
            if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
                total_memory = (uint64_t)kb * 1024ULL;
                break;
            }
        }
    }
    
    fclose(fp);
    return total_memory;
}

/*
 * get_linux_memory_info()
 *     Get detailed memory information on Linux using /proc/meminfo
 */
int get_linux_memory_info(system_memory_info_t *info)
{
    FILE *fp;
    char line[256];
    uint64_t mem_total = 0, mem_free = 0, mem_available = 0;
    uint64_t buffers = 0, cached = 0;
    
    if (!info)
        return -1;
    
    clear_memory_info(info);
    
    fp = fopen("/proc/meminfo", "r");
    if (!fp)
        return -1;
    
    while (fgets(line, sizeof(line), fp)) {
        unsigned long kb;
        
        if (strncmp(line, "MemTotal:", 9) == 0) {
            if (sscanf(line, "MemTotal: %lu kB", &kb) == 1)
                mem_total = (uint64_t)kb * 1024ULL;
        } else if (strncmp(line, "MemFree:", 8) == 0) {
            if (sscanf(line, "MemFree: %lu kB", &kb) == 1)
                mem_free = (uint64_t)kb * 1024ULL;
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1)
                mem_available = (uint64_t)kb * 1024ULL;
        } else if (strncmp(line, "Buffers:", 8) == 0) {
            if (sscanf(line, "Buffers: %lu kB", &kb) == 1)
                buffers = (uint64_t)kb * 1024ULL;
        } else if (strncmp(line, "Cached:", 7) == 0) {
            if (sscanf(line, "Cached: %lu kB", &kb) == 1)
                cached = (uint64_t)kb * 1024ULL;
        }
    }
    
    fclose(fp);
    
    if (mem_total == 0)
        return -1;
    
    /* Fill in the memory info structure */
    info->total_memory = mem_total;
    info->free_memory = mem_free;
    
    /* Use MemAvailable if available, otherwise calculate */
    if (mem_available > 0) {
        info->available_memory = mem_available;
    } else {
        /* Fallback calculation: free + buffers + cached */
        info->available_memory = mem_free + buffers + cached;
    }
    
    /* Calculate used memory */
    info->used_memory = mem_total - info->available_memory;
    
    /* Calculate usage percentage */
    if (info->total_memory > 0) {
        info->usage_percentage = ((double)info->used_memory / (double)info->total_memory) * 100.0;
    }
    
    /* Set platform flags */
    info->is_macos = false;
    info->is_apple_silicon = false;
    info->timestamp = get_current_timestamp_us();
    
    return 0;
}
#endif /* __linux__ */

/*
 * get_total_physical_memory()
 *     Get total physical memory in a cross-platform way
 */
uint64_t get_total_physical_memory(void)
{
#ifdef __APPLE__
    return get_macos_total_memory();
#elif defined(__linux__)
    return get_linux_total_memory();
#else
    /* Fallback using POSIX sysconf */
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    
    if (pages > 0 && page_size > 0) {
        return (uint64_t)pages * (uint64_t)page_size;
    }
    
    return 0;
#endif
}

/*
 * get_system_memory_info()
 *     Get comprehensive system memory information with caching
 */
int get_system_memory_info(system_memory_info_t *info)
{
    uint64_t current_time;
    int ret;
    
    if (!info)
        return -1;
    
    current_time = get_current_timestamp_us();
    
    /* Check if cached data is still valid */
    if (cache_timestamp > 0 && 
        (current_time - cache_timestamp) < CACHE_VALIDITY_US) {
        *info = cached_info;
        return 0;
    }
    
    /* Get fresh memory information */
#ifdef __APPLE__
    ret = get_macos_memory_info(info);
#elif defined(__linux__)
    ret = get_linux_memory_info(info);
#else
    /* Fallback implementation */
    clear_memory_info(info);
    info->total_memory = get_total_physical_memory();
    if (info->total_memory == 0)
        return -1;
    
    /* Basic estimation - this is a fallback */
    info->used_memory = info->total_memory / 2; /* Rough estimate */
    info->available_memory = info->total_memory - info->used_memory;
    info->usage_percentage = 50.0; /* Rough estimate */
    info->timestamp = current_time;
    ret = 0;
#endif
    
    if (ret == 0) {
        /* Update cache */
        cached_info = *info;
        cache_timestamp = current_time;
    }
    
    return ret;
}

/*
 * calculate_memory_usage_percent()
 *     Calculate current memory usage percentage with caching
 */
double calculate_memory_usage_percent(void)
{
    system_memory_info_t info;
    
    if (get_system_memory_info(&info) == 0) {
        return info.usage_percentage;
    }
    
    return -1.0; /* Error indicator */
}

/*
 * calculate_memory_usage_percent_realtime()
 *     Calculate current memory usage percentage without caching (always fresh)
 */
double calculate_memory_usage_percent_realtime(void)
{
    system_memory_info_t info;
    uint64_t old_cache_timestamp = cache_timestamp;
    
    /* Temporarily invalidate cache to force fresh read */
    cache_timestamp = 0;
    
    if (get_system_memory_info(&info) == 0) {
        /* Restore cache timestamp if it was valid */
        if (old_cache_timestamp > 0) {
            cache_timestamp = old_cache_timestamp;
        }
        return info.usage_percentage;
    }
    
    /* Restore cache timestamp on error */
    cache_timestamp = old_cache_timestamp;
    return -1.0; /* Error indicator */
}

/*
 * get_memory_usage_with_timestamp()
 *     Get memory usage percentage along with timestamp for precise tracking
 */
int get_memory_usage_with_timestamp(double *usage_percent, uint64_t *timestamp)
{
    system_memory_info_t info;
    
    if (!usage_percent || !timestamp)
        return -1;
    
    if (get_system_memory_info(&info) == 0) {
        *usage_percent = info.usage_percentage;
        *timestamp = info.timestamp;
        return 0;
    }
    
    return -1;
}

/*
 * invalidate_memory_cache()
 *     Force invalidation of memory cache for next read
 */
void invalidate_memory_cache(void)
{
    cache_timestamp = 0;
}

/*
 * adaptive_allocator_init()
 *     Initialize adaptive memory allocator
 */
int adaptive_allocator_init(adaptive_allocator_t *allocator)
{
    if (!allocator)
        return -1;
    
    (void)shim_memset(allocator, 0, sizeof(*allocator));
    
    /* Set default chunk size to 1MB */
    allocator->chunk_size = 1024 * 1024;
    allocator->chunks = NULL;
    allocator->total_allocated = 0;
    allocator->should_stop = false;
    
    /* Initialize mutex */
    if (pthread_mutex_init(&allocator->mutex, NULL) != 0) {
        return -1;
    }
    
    return 0;
}

/*
 * adaptive_allocator_cleanup()
 *     Clean up adaptive memory allocator and free all memory
 */
void adaptive_allocator_cleanup(adaptive_allocator_t *allocator)
{
    memory_chunk_t *chunk, *next;
    
    if (!allocator)
        return;
    
    pthread_mutex_lock(&allocator->mutex);
    
    /* Free all allocated chunks */
    chunk = allocator->chunks;
    while (chunk) {
        next = chunk->next;
        if (chunk->ptr) {
            munmap(chunk->ptr, chunk->size);
        }
        free(chunk);
        chunk = next;
    }
    
    allocator->chunks = NULL;
    allocator->total_allocated = 0;
    
    pthread_mutex_unlock(&allocator->mutex);
    pthread_mutex_destroy(&allocator->mutex);
}

/*
 * adaptive_allocator_get_allocated()
 *     Get total allocated memory size
 */
uint64_t adaptive_allocator_get_allocated(adaptive_allocator_t *allocator)
{
    uint64_t total;
    
    if (!allocator)
        return 0;
    
    pthread_mutex_lock(&allocator->mutex);
    total = allocator->total_allocated;
    pthread_mutex_unlock(&allocator->mutex);
    
    return total;
}

/*
 * allocate_memory_chunk()
 *     Allocate a single memory chunk using mmap
 */
static memory_chunk_t *allocate_memory_chunk(size_t size)
{
    memory_chunk_t *chunk;
    void *ptr;
    
    /* Allocate chunk structure */
    chunk = malloc(sizeof(memory_chunk_t));
    if (!chunk)
        return NULL;
    
    /* Allocate memory using mmap */
    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        free(chunk);
        return NULL;
    }
    
    /* Touch the memory to ensure it's allocated */
    (void)shim_memset(ptr, 0x55, size);
    
    chunk->ptr = ptr;
    chunk->size = size;
    chunk->next = NULL;
    
    return chunk;
}

/*
 * free_memory_chunk()
 *     Free a single memory chunk
 */
static void free_memory_chunk(memory_chunk_t *chunk)
{
    if (!chunk)
        return;
    
    if (chunk->ptr) {
        munmap(chunk->ptr, chunk->size);
    }
    free(chunk);
}

/*
 * adaptive_allocator_adjust_to_target()
 *     Adjust memory allocation to reach target bytes
 */
int adaptive_allocator_adjust_to_target(adaptive_allocator_t *allocator, uint64_t target_bytes)
{
    memory_chunk_t *chunk, *new_chunk;
    uint64_t current_allocated;
    int64_t diff;
    size_t chunks_to_add, chunks_to_remove;
    
    if (!allocator)
        return -1;
    
    pthread_mutex_lock(&allocator->mutex);
    
    if (allocator->should_stop) {
        pthread_mutex_unlock(&allocator->mutex);
        return -1;
    }
    
    current_allocated = allocator->total_allocated;
    diff = (int64_t)target_bytes - (int64_t)current_allocated;
    
    if (diff > 0) {
        /* Need to allocate more memory */
        chunks_to_add = (size_t)diff / allocator->chunk_size;
        if ((size_t)diff % allocator->chunk_size > 0) {
            chunks_to_add++;
        }
        
        /* Allocate new chunks */
        for (size_t i = 0; i < chunks_to_add; i++) {
            new_chunk = allocate_memory_chunk(allocator->chunk_size);
            if (!new_chunk) {
                /* Allocation failed, stop here */
                break;
            }
            
            /* Add to linked list */
            new_chunk->next = allocator->chunks;
            allocator->chunks = new_chunk;
            allocator->total_allocated += allocator->chunk_size;
        }
    } else if (diff < 0) {
        /* Need to free some memory */
        chunks_to_remove = (size_t)(-diff) / allocator->chunk_size;
        if ((size_t)(-diff) % allocator->chunk_size > 0) {
            chunks_to_remove++;
        }
        
        /* Free chunks from the beginning of the list */
        for (size_t i = 0; i < chunks_to_remove && allocator->chunks; i++) {
            chunk = allocator->chunks;
            allocator->chunks = chunk->next;
            allocator->total_allocated -= chunk->size;
            free_memory_chunk(chunk);
        }
    }
    
    pthread_mutex_unlock(&allocator->mutex);
    return 0;
}

/*
 * precise_control_context_init()
 *     Initialize precise memory control context
 */
int precise_control_context_init(precise_control_context_t *ctx, 
                               double target_percentage, 
                               uint64_t duration_seconds,
                               double accuracy_threshold)
{
    if (!ctx || target_percentage < 0.0 || target_percentage > 95.0)
        return -1;
    
    (void)shim_memset(ctx, 0, sizeof(*ctx));
    
    /* Set basic parameters */
    ctx->target_percentage = target_percentage;
    ctx->duration_seconds = duration_seconds;
    ctx->accuracy_threshold = accuracy_threshold;
    
    /* Get total system memory */
    ctx->total_memory = get_total_physical_memory();
    if (ctx->total_memory == 0)
        return -1;
    
    /* Calculate target bytes */
    ctx->target_bytes = (uint64_t)((double)ctx->total_memory * target_percentage / 100.0);
    
    /* Initialize stabilization config */
    ctx->stab_config.stabilization_window = 10;
    ctx->stab_config.stabilization_threshold = 2.0; /* More strict than accuracy_threshold */
    ctx->stab_config.min_stable_duration = 5;
    ctx->stab_config.monitor_interval_ms = 1000;
    ctx->stab_config.adjustment_interval_ms = 500;
    ctx->stab_config.max_stabilization_time = 60;
    
    /* Initialize allocator */
    if (adaptive_allocator_init(&ctx->allocator) != 0)
        return -1;
    
    /* Initialize statistics */
    ctx->stats.target_percentage = target_percentage;
    ctx->stats.target_bytes = ctx->target_bytes;
    ctx->stats.duration_seconds = duration_seconds;
    ctx->stats.start_time = get_current_timestamp_us();
    ctx->stats.required_stable_samples = ctx->stab_config.stabilization_window;
    ctx->stats.max_deviation = -1000.0; /* Initialize to impossible value */
    ctx->stats.min_deviation = 1000.0;  /* Initialize to impossible value */
    
    /* Initialize state */
    ctx->is_stabilized = false;
    ctx->stabilization_start_time = ctx->stats.start_time;
    ctx->sample_index = 0;
    
    return 0;
}

/*
 * precise_control_context_cleanup()
 *     Clean up precise memory control context
 */
void precise_control_context_cleanup(precise_control_context_t *ctx)
{
    if (!ctx)
        return;
    
    adaptive_allocator_cleanup(&ctx->allocator);
    (void)shim_memset(ctx, 0, sizeof(*ctx));
}

/*
 * check_stabilization()
 *     Check if memory usage has stabilized within threshold
 */
bool check_stabilization(precise_control_context_t *ctx)
{
    uint32_t i, stable_count;
    double deviation;
    uint64_t current_time;
    
    if (!ctx)
        return false;
    
    /* If already stabilized, check if we should exit stabilization */
    if (ctx->is_stabilized) {
        current_time = get_current_timestamp_us();
        if (ctx->stable_period_start_time > 0 && 
            (current_time - ctx->stable_period_start_time) >= (ctx->duration_seconds * 1000000ULL)) {
            return true; /* Stable period completed */
        }
        return false; /* Still in stable period */
    }
    
    /* Get current memory usage */
    if (get_system_memory_info(&ctx->current_info) != 0)
        return false;
    
    /* Add current usage to samples */
    ctx->recent_usage_samples[ctx->sample_index] = ctx->current_info.usage_percentage;
    ctx->sample_index = (ctx->sample_index + 1) % ctx->stab_config.stabilization_window;
    
    /* Update statistics */
    ctx->stats.total_sample_count++;
    ctx->stats.sum_usage_percentage += ctx->current_info.usage_percentage;
    
    deviation = ctx->current_info.usage_percentage - ctx->target_percentage;
    if (ctx->stats.max_deviation < -999.0 || deviation > ctx->stats.max_deviation) {
        ctx->stats.max_deviation = deviation;
    }
    if (ctx->stats.min_deviation > 999.0 || deviation < ctx->stats.min_deviation) {
        ctx->stats.min_deviation = deviation;
    }
    
    if (fabs(deviation) <= ctx->accuracy_threshold) {
        ctx->stats.within_threshold_count++;
    }
    
    /* Check if we have enough samples */
    if (ctx->stats.total_sample_count < ctx->stab_config.stabilization_window)
        return false;
    
    /* Check if all recent samples are within stabilization threshold */
    stable_count = 0;
    for (i = 0; i < ctx->stab_config.stabilization_window; i++) {
        deviation = fabs(ctx->recent_usage_samples[i] - ctx->target_percentage);
        if (deviation <= ctx->stab_config.stabilization_threshold) {
            stable_count++;
        }
    }
    
    /* Check timeout */
    current_time = get_current_timestamp_us();
    if ((current_time - ctx->stabilization_start_time) >= 
        (ctx->stab_config.max_stabilization_time * 1000000ULL)) {
        /* Timeout reached, consider it stabilized anyway */
        ctx->is_stabilized = true;
        ctx->stats.stabilization_time = current_time;
        ctx->stable_period_start_time = current_time;
        ctx->stats.stable_period_start = current_time;
        return false; /* Continue to stable period */
    }
    
    /* Check if stabilized */
    if (stable_count >= ctx->stab_config.stabilization_window) {
        ctx->is_stabilized = true;
        ctx->stats.is_stabilized = true;
        ctx->stats.stabilization_time = current_time;
        ctx->stable_period_start_time = current_time;
        ctx->stats.stable_period_start = current_time;
        ctx->stats.consecutive_stable_samples = stable_count;
        return false; /* Continue to stable period */
    }
    
    return false;
}

/*
 * adjust_memory_allocation()
 *     Adjust memory allocation based on current usage and target
 */
int adjust_memory_allocation(precise_control_context_t *ctx)
{
    double current_usage, target_usage, diff_percentage;
    uint64_t allocated, adjustment;
    int ret;
    
    if (!ctx)
        return -1;
    
    /* Get current memory info */
    if (get_system_memory_info(&ctx->current_info) != 0)
        return -1;
    
    current_usage = ctx->current_info.usage_percentage;
    target_usage = ctx->target_percentage;
    diff_percentage = target_usage - current_usage;
    
    /* Calculate adjustment needed */
    allocated = adaptive_allocator_get_allocated(&ctx->allocator);
    
    if (diff_percentage > 0.5) {
        /* Need to allocate more memory */
        adjustment = (uint64_t)((double)ctx->total_memory * diff_percentage / 100.0);
        ret = adaptive_allocator_adjust_to_target(&ctx->allocator, allocated + adjustment);
    } else if (diff_percentage < -0.5) {
        /* Need to free some memory */
        adjustment = (uint64_t)((double)ctx->total_memory * (-diff_percentage) / 100.0);
        if (adjustment < allocated) {
            ret = adaptive_allocator_adjust_to_target(&ctx->allocator, allocated - adjustment);
        } else {
            ret = adaptive_allocator_adjust_to_target(&ctx->allocator, 0);
        }
    } else {
        /* Within acceptable range, no adjustment needed */
        ret = 0;
    }
    
    if (ret == 0) {
        ctx->stats.adjustment_count++;
        ctx->stats.total_allocated = adaptive_allocator_get_allocated(&ctx->allocator);
        if (ctx->stats.total_allocated > ctx->stats.peak_allocated) {
            ctx->stats.peak_allocated = ctx->stats.total_allocated;
        }
    } else {
        ctx->stats.allocation_failures++;
    }
    
    return ret;
}

/*
 * log_phase_transition()
 *     Log phase transition information
 */
void log_phase_transition(const precise_control_context_t *ctx, const char *phase)
{
    if (!ctx || !phase)
        return;
    
    /* This would normally use stress-ng's logging system */
    /* For now, we'll just track the transitions in stats */
    (void)ctx;
    (void)phase;
}

/*
 * log_final_statistics()
 *     Log final statistics and results
 */
void log_final_statistics(precise_control_context_t *ctx)
{
    double stabilization_duration, stable_duration, total_duration;
    
    if (!ctx)
        return;
    
    /* Calculate durations */
    if (ctx->stats.stabilization_time > ctx->stats.start_time) {
        stabilization_duration = (double)(ctx->stats.stabilization_time - ctx->stats.start_time) / 1000000.0;
    } else {
        stabilization_duration = 0.0;
    }
    
    if (ctx->stats.stable_period_end > ctx->stats.stable_period_start) {
        stable_duration = (double)(ctx->stats.stable_period_end - ctx->stats.stable_period_start) / 1000000.0;
    } else {
        stable_duration = 0.0;
    }
    
    if (ctx->stats.end_time > ctx->stats.start_time) {
        total_duration = (double)(ctx->stats.end_time - ctx->stats.start_time) / 1000000.0;
    } else {
        total_duration = 0.0;
    }
    
    /* Calculate accuracy percentages */
    if (ctx->stats.total_sample_count > 0) {
        ctx->stats.overall_accuracy_percentage = 
            ((double)ctx->stats.within_threshold_count / (double)ctx->stats.total_sample_count) * 100.0;
    }
    
    if (ctx->stats.stable_sample_count > 0) {
        ctx->stats.stable_period_accuracy_percentage = 
            ((double)ctx->stats.stable_within_threshold_count / (double)ctx->stats.stable_sample_count) * 100.0;
    }
    
    if (ctx->stats.total_sample_count > 0) {
        ctx->stats.stable_period_avg_usage = ctx->stats.sum_usage_percentage / (double)ctx->stats.total_sample_count;
    }
    
    /* This would normally output to stress-ng's logging system */
    /* For now, the statistics are stored in the context for later retrieval */
    (void)stabilization_duration;
    (void)stable_duration;
    (void)total_duration;
}