#include "cache.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstring>

PagedCache::PagedCache() : page_bitmap(TOTAL_PAGES, false), free_list_head(nullptr) {
    std::cout << "Initialized cache with " << TOTAL_PAGES << " pages (" 
              << (TOTAL_CACHE_SIZE / (1024 * 1024)) << " MB)" << std::endl;
    
    // Initialize free list with entire memory space
    free_list_head = std::make_shared<FreeBlock>(0, TOTAL_PAGES);
    std::cout << "Initialized linked list-based memory management to prevent fragmentation" << std::endl;
}

size_t PagedCache::calculate_pages_needed(const std::string& value) {
    size_t data_size = value.size();
    return (data_size + PAGE_SIZE - 1) / PAGE_SIZE;  // Ceiling division
}

// New linked list-based allocation method
std::shared_ptr<MemoryBlock> PagedCache::allocate_pages_linked(size_t num_pages) {
    if (num_pages == 0) return nullptr;
    
    std::shared_ptr<MemoryBlock> allocated_blocks = nullptr;
    std::shared_ptr<MemoryBlock> current_block = nullptr;
    size_t pages_allocated = 0;
    
    std::shared_ptr<FreeBlock> prev = nullptr;
    std::shared_ptr<FreeBlock> current = free_list_head;
    
    // Allocate pages from available free blocks
    while (current && pages_allocated < num_pages) {
        size_t pages_to_take = std::min(current->num_pages, num_pages - pages_allocated);
        
        // Create memory blocks for this allocation
        for (size_t i = 0; i < pages_to_take; ++i) {
            auto new_block = std::make_shared<MemoryBlock>(current->start_page + i);
            
            if (!allocated_blocks) {
                allocated_blocks = new_block;
                current_block = new_block;
            } else {
                current_block->next = new_block;
                current_block = new_block;
            }
            
            // Mark page as allocated
            page_bitmap[current->start_page + i] = true;
        }
        
        pages_allocated += pages_to_take;
        
        // Update free block
        if (pages_to_take == current->num_pages) {
            // Remove this free block completely
            if (prev) {
                prev->next = current->next;
            } else {
                free_list_head = current->next;
            }
            current = current->next;
        } else {
            // Shrink this free block
            current->start_page += pages_to_take;
            current->num_pages -= pages_to_take;
            prev = current;
            current = current->next;
        }
    }
    
    if (pages_allocated == num_pages) {
        return allocated_blocks;
    } else {
        // Not enough memory, deallocate what we allocated
        if (allocated_blocks) {
            deallocate_pages_linked(allocated_blocks);
        }
        return nullptr;
    }
}

void PagedCache::deallocate_pages_linked(std::shared_ptr<MemoryBlock> blocks) {
    std::shared_ptr<MemoryBlock> current = blocks;
    
    while (current) {
        // Mark page as free
        page_bitmap[current->page_index] = false;
        
        // Add to free list as individual pages (will be coalesced later)
        add_to_free_list(current->page_index, 1);
        
        current = current->next;
    }
    
    // Coalesce adjacent free blocks to reduce fragmentation
    coalesce_free_blocks();
}

void PagedCache::add_to_free_list(size_t start_page, size_t num_pages) {
    auto new_block = std::make_shared<FreeBlock>(start_page, num_pages);
    
    // Insert in sorted order by start_page for easier coalescing
    if (!free_list_head || start_page < free_list_head->start_page) {
        new_block->next = free_list_head;
        free_list_head = new_block;
    } else {
        std::shared_ptr<FreeBlock> current = free_list_head;
        while (current->next && current->next->start_page < start_page) {
            current = current->next;
        }
        new_block->next = current->next;
        current->next = new_block;
    }
}

void PagedCache::coalesce_free_blocks() {
    if (!free_list_head) return;
    
    std::shared_ptr<FreeBlock> current = free_list_head;
    
    while (current && current->next) {
        // Check if current block can be merged with next block
        if (current->start_page + current->num_pages == current->next->start_page) {
            // Merge blocks
            current->num_pages += current->next->num_pages;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

// Fallback method for allocation from free list (tries to find best fit)
std::shared_ptr<MemoryBlock> PagedCache::allocate_from_free_list(size_t num_pages) {
    return allocate_pages_linked(num_pages);
}

void PagedCache::update_lru(const std::string& key) {
    // Remove key from current position in LRU list
    auto it = std::find(lru_order.begin(), lru_order.end(), key);
    if (it != lru_order.end()) {
        lru_order.erase(it);
    }
    // Add to front (most recently used)
    lru_order.push_front(key);
}

bool PagedCache::evict_lru_if_needed(size_t pages_needed, const std::string& requesting_client) {
    size_t free_pages = get_free_pages();
    
    if (free_pages >= pages_needed) {
        return true;  // No eviction needed
    }
    
    size_t pages_to_free = pages_needed - free_pages;
    size_t pages_freed = 0;
    
    // Try to evict from least recently used, but only from other clients
    auto it = lru_order.rbegin();
    while (it != lru_order.rend() && pages_freed < pages_to_free) {
        const std::string& lru_key = *it;
        auto entry_it = cache_map.find(lru_key);
        
        if (entry_it != cache_map.end() && entry_it->second->client_id != requesting_client) {
            size_t entry_pages = entry_it->second->total_pages;
            // Use linked list deallocation instead of contiguous deallocation
            deallocate_pages_linked(entry_it->second->memory_blocks);
            cache_map.erase(entry_it);
            
            it = std::reverse_iterator(lru_order.erase(std::next(it).base()));
            pages_freed += entry_pages;
        } else {
            ++it;
        }
    }
    
    return pages_freed >= pages_to_free;
}

PagedCache::OperationResult PagedCache::add_key(const std::string& key, const std::string& value, const std::string& client_id) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    // Check if key already exists
    if (cache_map.find(key) != cache_map.end()) {
        return OperationResult::KEY_EXISTS;
    }
    
    size_t pages_needed = calculate_pages_needed(value);
    
    // Try to allocate using linked list approach (no fragmentation)
    std::shared_ptr<MemoryBlock> allocated_blocks = allocate_pages_linked(pages_needed);
    
    if (!allocated_blocks) {
        // Try LRU eviction
        if (!evict_lru_if_needed(pages_needed, client_id)) {
            return OperationResult::NO_SPACE;
        }
        // Try again after eviction
        allocated_blocks = allocate_pages_linked(pages_needed);
        if (!allocated_blocks) {
            return OperationResult::NO_SPACE;
        }
    }
    
    // Create cache entry with linked list of memory blocks
    cache_map[key] = std::make_unique<CacheEntry>(key, value, allocated_blocks, pages_needed, client_id);
    update_lru(key);
    
    return OperationResult::SUCCESS;
}

PagedCache::OperationResult PagedCache::update_key(const std::string& key, const std::string& value, const std::string& client_id) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    auto it = cache_map.find(key);
    if (it == cache_map.end()) {
        return OperationResult::KEY_NOT_FOUND;
    }
    
    // Check client ownership
    if (it->second->client_id != client_id) {
        return OperationResult::CLIENT_MISMATCH;
    }
    
    size_t new_pages_needed = calculate_pages_needed(value);
    size_t old_pages = it->second->total_pages;
    
    if (new_pages_needed <= old_pages) {
        // Can use existing allocation (potentially with some waste, but no reallocation needed)
        it->second->value = value;
        it->second->last_access = std::chrono::steady_clock::now();
        
        // If we need fewer pages, we could deallocate some blocks, but for simplicity
        // we'll keep the allocation (trade memory for performance)
        // This avoids frequent reallocation cycles
        
        update_lru(key);
        return OperationResult::SUCCESS;
    } else {
        // Need more space - deallocate old and allocate new
        std::shared_ptr<MemoryBlock> old_blocks = it->second->memory_blocks;
        deallocate_pages_linked(old_blocks);
        
        // Try to allocate new space
        std::shared_ptr<MemoryBlock> new_blocks = allocate_pages_linked(new_pages_needed);
        if (!new_blocks) {
            // Try LRU eviction
            if (!evict_lru_if_needed(new_pages_needed, client_id)) {
                // Restore old allocation by trying to allocate the old size again
                std::shared_ptr<MemoryBlock> restore_blocks = allocate_pages_linked(old_pages);
                if (restore_blocks) {
                    it->second->memory_blocks = restore_blocks;
                }
                return OperationResult::NO_SPACE;
            }
            new_blocks = allocate_pages_linked(new_pages_needed);
            if (!new_blocks) {
                // Still can't allocate, try to restore old allocation
                std::shared_ptr<MemoryBlock> restore_blocks = allocate_pages_linked(old_pages);
                if (restore_blocks) {
                    it->second->memory_blocks = restore_blocks;
                }
                return OperationResult::NO_SPACE;
            }
        }
        
        // Update with new allocation
        it->second->value = value;
        it->second->memory_blocks = new_blocks;
        it->second->total_pages = new_pages_needed;
        it->second->last_access = std::chrono::steady_clock::now();
        
        update_lru(key);
        return OperationResult::SUCCESS;
    }
}

PagedCache::OperationResult PagedCache::get_key(const std::string& key, std::string& value, const std::string& client_id) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    auto it = cache_map.find(key);
    if (it == cache_map.end()) {
        return OperationResult::KEY_NOT_FOUND;
    }
    
    // Check client ownership
    if (it->second->client_id != client_id) {
        return OperationResult::CLIENT_MISMATCH;
    }
    
    value = it->second->value;
    it->second->last_access = std::chrono::steady_clock::now();
    update_lru(key);
    
    return OperationResult::SUCCESS;
}

PagedCache::OperationResult PagedCache::delete_key(const std::string& key, const std::string& client_id) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    auto it = cache_map.find(key);
    if (it == cache_map.end()) {
        return OperationResult::KEY_NOT_FOUND;
    }
    
    // Check client ownership
    if (it->second->client_id != client_id) {
        return OperationResult::CLIENT_MISMATCH;
    }
    
    // Deallocate pages using linked list approach
    deallocate_pages_linked(it->second->memory_blocks);
    
    // Remove from LRU list
    auto lru_it = std::find(lru_order.begin(), lru_order.end(), key);
    if (lru_it != lru_order.end()) {
        lru_order.erase(lru_it);
    }
    
    // Remove from cache
    cache_map.erase(it);
    
    return OperationResult::SUCCESS;
}

size_t PagedCache::get_free_pages() const {
    return std::count(page_bitmap.begin(), page_bitmap.end(), false);
}

// Legacy methods for backward compatibility (now wrapper around linked list methods)
bool PagedCache::find_contiguous_pages(size_t num_pages, size_t& start_page) {
    // This method is now deprecated but kept for compatibility
    // Try to find a single free block that can satisfy the request
    std::shared_ptr<FreeBlock> current = free_list_head;
    while (current) {
        if (current->num_pages >= num_pages) {
            start_page = current->start_page;
            return true;
        }
        current = current->next;
    }
    return false;
}

void PagedCache::allocate_pages(size_t start_page, size_t num_pages) {
    // Legacy method - mark pages as allocated in bitmap
    for (size_t i = start_page; i < start_page + num_pages; ++i) {
        page_bitmap[i] = true;
    }
    
    // Remove from free list
    std::shared_ptr<FreeBlock> prev = nullptr;
    std::shared_ptr<FreeBlock> current = free_list_head;
    
    while (current) {
        if (current->start_page <= start_page && 
            start_page + num_pages <= current->start_page + current->num_pages) {
            
            // Split the free block
            size_t before_pages = start_page - current->start_page;
            size_t after_pages = (current->start_page + current->num_pages) - (start_page + num_pages);
            
            if (before_pages > 0) {
                // Create block before allocated region
                auto before_block = std::make_shared<FreeBlock>(current->start_page, before_pages);
                before_block->next = current->next;
                if (prev) {
                    prev->next = before_block;
                } else {
                    free_list_head = before_block;
                }
                prev = before_block;
            }
            
            if (after_pages > 0) {
                // Create block after allocated region
                auto after_block = std::make_shared<FreeBlock>(start_page + num_pages, after_pages);
                after_block->next = current->next;
                if (prev) {
                    prev->next = after_block;
                } else {
                    free_list_head = after_block;
                }
            } else if (!before_pages) {
                // Remove entire block
                if (prev) {
                    prev->next = current->next;
                } else {
                    free_list_head = current->next;
                }
            }
            
            break;
        }
        prev = current;
        current = current->next;
    }
}

void PagedCache::deallocate_pages(size_t start_page, size_t num_pages) {
    // Legacy method - mark pages as free and add to free list
    for (size_t i = start_page; i < start_page + num_pages; ++i) {
        page_bitmap[i] = false;
    }
    
    add_to_free_list(start_page, num_pages);
    coalesce_free_blocks();
}

// New method to get fragmentation statistics
void PagedCache::get_fragmentation_stats(size_t& free_blocks_count, size_t& largest_free_block, double& fragmentation_ratio) const {
    free_blocks_count = 0;
    largest_free_block = 0;
    size_t total_free_pages = 0;
    
    std::shared_ptr<FreeBlock> current = free_list_head;
    while (current) {
        free_blocks_count++;
        largest_free_block = std::max(largest_free_block, current->num_pages);
        total_free_pages += current->num_pages;
        current = current->next;
    }
    
    // Fragmentation ratio: (1 - largest_block/total_free) * 100
    // 0% = no fragmentation, 100% = maximum fragmentation
    if (total_free_pages > 0) {
        fragmentation_ratio = (1.0 - static_cast<double>(largest_free_block) / total_free_pages) * 100.0;
    } else {
        fragmentation_ratio = 0.0;
    }
}