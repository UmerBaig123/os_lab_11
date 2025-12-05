#ifndef CACHE_H
#define CACHE_H

#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <list>
#include <mutex>
#include <chrono>

// Cache configuration constants
const size_t TOTAL_CACHE_SIZE = 2ULL * 1024 * 1024 * 1024;  // 2 GB
const size_t PAGE_SIZE = 40 * 1024;  // 40 KB
const size_t TOTAL_PAGES = TOTAL_CACHE_SIZE / PAGE_SIZE;  // ~52,428 pages

// Memory block structure for linked list-based allocation
struct MemoryBlock {
    size_t page_index;
    std::shared_ptr<MemoryBlock> next;
    
    MemoryBlock(size_t idx) : page_index(idx), next(nullptr) {}
};

// Free block structure for managing available memory
struct FreeBlock {
    size_t start_page;
    size_t num_pages;
    std::shared_ptr<FreeBlock> next;
    
    FreeBlock(size_t start, size_t count) : start_page(start), num_pages(count), next(nullptr) {}
};

struct CacheEntry {
    std::string key;
    std::string value;
    std::shared_ptr<MemoryBlock> memory_blocks;  // Linked list of memory blocks
    size_t total_pages;
    std::chrono::steady_clock::time_point last_access;
    std::string client_id;
    
    CacheEntry(const std::string& k, const std::string& v, std::shared_ptr<MemoryBlock> blocks, size_t count, const std::string& client)
        : key(k), value(v), memory_blocks(blocks), total_pages(count), client_id(client) {
        last_access = std::chrono::steady_clock::now();
    }
};

class PagedCache {
private:
    std::vector<bool> page_bitmap;  // Track allocated pages
    std::unordered_map<std::string, std::unique_ptr<CacheEntry>> cache_map;
    std::list<std::string> lru_order;  // For LRU eviction
    std::mutex cache_mutex;
    
    // Free list management for avoiding fragmentation
    std::shared_ptr<FreeBlock> free_list_head;
    
    size_t calculate_pages_needed(const std::string& value);
    
    // New linked list-based allocation methods
    std::shared_ptr<MemoryBlock> allocate_pages_linked(size_t num_pages);
    void deallocate_pages_linked(std::shared_ptr<MemoryBlock> blocks);
    void add_to_free_list(size_t start_page, size_t num_pages);
    std::shared_ptr<MemoryBlock> allocate_from_free_list(size_t num_pages);
    void coalesce_free_blocks();
    
    // Legacy methods for backward compatibility (will be updated)
    bool find_contiguous_pages(size_t num_pages, size_t& start_page);
    void allocate_pages(size_t start_page, size_t num_pages);
    void deallocate_pages(size_t start_page, size_t num_pages);
    
    void update_lru(const std::string& key);
    bool evict_lru_if_needed(size_t pages_needed, const std::string& requesting_client);
    
public:
    PagedCache();
    ~PagedCache() = default;
    
    enum class OperationResult {
        SUCCESS,
        KEY_EXISTS,
        KEY_NOT_FOUND,
        NO_SPACE,
        CLIENT_MISMATCH
    };
    
    OperationResult add_key(const std::string& key, const std::string& value, const std::string& client_id);
    OperationResult update_key(const std::string& key, const std::string& value, const std::string& client_id);
    OperationResult get_key(const std::string& key, std::string& value, const std::string& client_id);
    OperationResult delete_key(const std::string& key, const std::string& client_id);
    
    size_t get_free_pages() const;
    size_t get_total_pages() const { return TOTAL_PAGES; }
    
    // New method for analyzing memory fragmentation
    void get_fragmentation_stats(size_t& free_blocks_count, size_t& largest_free_block, double& fragmentation_ratio) const;
};

#endif // CACHE_H