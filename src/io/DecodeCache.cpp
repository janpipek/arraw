#include "pipeline/LoadResult.h"
#include "io/DecodeCache.h"

DecodeCache::DecodeCache(size_t byteBudget)
    : budget(byteBudget) {}

size_t DecodeCache::sizeOf(const LoadResult& r) {
    return (r.fullRes.data.size() + r.preview.data.size() + r.sensorClipFullRes.data.size()
            + r.sensorClipPreview.data.size())
           * sizeof(float);
}

void DecodeCache::insert(const QString& key, LoadResult result) {
    if (auto it = index.find(key); it != index.end()) {
        totalBytes -= it.value()->bytes;
        entries.erase(it.value());
        index.erase(it);
    }

    const size_t bytes = sizeOf(result);
    entries.push_front({key, std::move(result), bytes});
    index.insert(key, entries.begin());
    totalBytes += bytes;

    evictToBudget();
}

const LoadResult* DecodeCache::get(const QString& key) {
    auto it = index.find(key);
    if (it == index.end())
        return nullptr;
    entries.splice(entries.begin(), entries, it.value()); // bump to most-recently-used
    return &it.value()->result;
}

void DecodeCache::evictToBudget() {
    while (totalBytes > budget) {
        // Evict the least-recently-used entry that is not pinned (scan from the
        // back). If only the pinned entry remains, stop — it stays even over budget.
        auto victim = entries.end();
        for (auto it = entries.end(); it != entries.begin();) {
            --it;
            if (it->key != pinnedKey) {
                victim = it;
                break;
            }
        }
        if (victim == entries.end())
            break;

        totalBytes -= victim->bytes;
        index.remove(victim->key);
        entries.erase(victim);
    }
}
