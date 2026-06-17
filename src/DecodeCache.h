#pragma once
#include "ImagePipeline.h"
#include <list>
#include <QHash>
#include <QString>

// Session-scoped, in-memory MRU cache of decoded LoadResults, keyed by a
// caller-supplied string (path|size|mtime). Pure logic: it never touches the
// filesystem, so it is fully unit-testable with synthetic results. Bounded by a
// total byte budget; least-recently-used entries are evicted on insert. One key
// may be pinned (the current image) and is never evicted.
class DecodeCache {
public:
    explicit DecodeCache(size_t byteBudget);

    // Insert or replace the entry for key, then evict LRU entries until within
    // budget.
    void insert(const QString& key, LoadResult result);

    // Hit: pointer to the cached result (marked most-recently-used). Miss:
    // nullptr. The pointer is valid until the next insert.
    const LoadResult* get(const QString& key);

    // Mark key as the current image so it is never evicted. At most one key is
    // pinned; pinning a new key unpins the previous one.
    void pin(const QString& key) { pinnedKey = key; }

    // Total bytes currently held (sum of entry sizes).
    size_t byteSize() const { return totalBytes; }

private:
    struct Entry {
        QString key;
        LoadResult result;
        size_t bytes;
    };

    static size_t sizeOf(const LoadResult& r);
    void evictToBudget();

    std::list<Entry> entries;                          // most- to least-recently-used
    QHash<QString, std::list<Entry>::iterator> index;  // key → position in entries
    size_t totalBytes = 0;
    size_t budget;
    QString pinnedKey;
};
