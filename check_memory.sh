#!/bin/bash

# Get memory info using vm_stat
vm_stat_output=$(vm_stat)

# Extract values (page size is 16384 bytes on Apple Silicon)
page_size=16384

# Extract page counts
pages_free=$(echo "$vm_stat_output" | grep "Pages free:" | awk '{print $3}' | tr -d '.')
pages_active=$(echo "$vm_stat_output" | grep "Pages active:" | awk '{print $3}' | tr -d '.')
pages_inactive=$(echo "$vm_stat_output" | grep "Pages inactive:" | awk '{print $3}' | tr -d '.')
pages_wired=$(echo "$vm_stat_output" | grep "Pages wired down:" | awk '{print $4}' | tr -d '.')
pages_compressed=$(echo "$vm_stat_output" | grep "Pages stored in compressor:" | awk '{print $5}' | tr -d '.')

# Get total memory using sysctl
total_memory=$(sysctl -n hw.memsize)

# Calculate memory usage (active + inactive + wired + compressed)
used_memory=$(( (pages_active + pages_inactive + pages_wired + pages_compressed) * page_size ))
available_memory=$(( pages_free * page_size + pages_inactive * page_size ))
actual_used=$(( total_memory - available_memory ))

usage_percent=$(echo "scale=2; $actual_used * 100 / $total_memory" | bc -l)

echo "=== Memory Status ==="
echo "Total Memory: $(echo "scale=2; $total_memory / 1024 / 1024 / 1024" | bc -l) GB"
echo "Free Pages: $pages_free"
echo "Active Pages: $pages_active" 
echo "Inactive Pages: $pages_inactive"
echo "Wired Pages: $pages_wired"
echo "Compressed Pages: $pages_compressed"
echo ""
echo "Used Memory: $(echo "scale=2; $actual_used / 1024 / 1024 / 1024" | bc -l) GB"
echo "Available Memory: $(echo "scale=2; $available_memory / 1024 / 1024 / 1024" | bc -l) GB"
echo "Memory Usage: ${usage_percent}%"