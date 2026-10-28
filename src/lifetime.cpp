#include "dvv/base/lifetime.h"

#if defined(__GNUC__) || defined(__clang__)
#include <cstdlib>

#include <cxxabi.h>
#endif

namespace dvv {

namespace detail {

std::string demangle(const char* name) {
#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* readable = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && readable != nullptr) {
        std::string result(readable);
        std::free(readable);
        return result;
    }
    return name;
#else
    std::string_view text = name;
    for (std::string_view prefix : {"class ", "struct "}) {
        if (text.starts_with(prefix)) {
            text.remove_prefix(prefix.size());
        }
    }
    return std::string(text);
#endif
}

}  // namespace detail

LifetimeRegistry::LifetimeRegistry() = default;

LifetimeRegistry& LifetimeRegistry::instance() noexcept {
    // Intentionally leaked; out of memory at start-up is fatal anyway.
    static auto* registry = new LifetimeRegistry();  // NOLINT(bugprone-unhandled-exception-at-new)
    return *registry;
}

void LifetimeRegistry::setDetailed(bool detailed) noexcept {
    detailed_.store(detailed, std::memory_order_relaxed);
}

bool LifetimeRegistry::detailed() const noexcept {
    return detailed_.load(std::memory_order_relaxed);
}

std::size_t LifetimeRegistry::liveCount() const noexcept {
    return live_count_.load(std::memory_order_relaxed);
}

TypeRecord& LifetimeRegistry::typeRecord(std::string_view name) {
    std::lock_guard lock(types_mutex_);
    for (const auto& record : types_) {
        if (record->name == name) {
            return *record;
        }
    }
    auto record = std::make_unique<TypeRecord>();
    record->name = std::string(name);
    types_.push_back(std::move(record));
    return *types_.back();
}

LifetimeToken LifetimeRegistry::makeToken(TypeRecord& type, ObjectId owner,
                                          std::source_location where) noexcept {
    LifetimeToken token;
    token.type_ = &type;
    token.id_ = next_id_.fetch_add(1, std::memory_order_relaxed);
    token.owner_ = owner;
    if (detailed()) {
        token.where_ = where;
        token.time_ = std::chrono::system_clock::now();
    }
    return token;
}

void LifetimeRegistry::link(ManagedObject& object) noexcept {
    Shard& shard = shards_[object.id_ % SHARD_COUNT];
    {
        std::lock_guard lock(shard.mutex);
        object.next_ = shard.head;
        if (shard.head != nullptr) {
            shard.head->prev_ = &object;
        }
        shard.head = &object;
    }
    object.type_->created.fetch_add(1, std::memory_order_relaxed);
    object.type_->live.fetch_add(1, std::memory_order_relaxed);
    live_count_.fetch_add(1, std::memory_order_relaxed);
}

void LifetimeRegistry::unlink(ManagedObject& object) noexcept {
    Shard& shard = shards_[object.id_ % SHARD_COUNT];
    {
        std::lock_guard lock(shard.mutex);
        if (object.prev_ != nullptr) {
            object.prev_->next_ = object.next_;
        } else {
            shard.head = object.next_;
        }
        if (object.next_ != nullptr) {
            object.next_->prev_ = object.prev_;
        }
    }
    object.type_->live.fetch_sub(1, std::memory_order_relaxed);
    live_count_.fetch_sub(1, std::memory_order_relaxed);
}

std::vector<LiveObjectInfo> LifetimeRegistry::liveObjects() const {
    std::vector<LiveObjectInfo> objects;
    for (const Shard& shard : shards_) {
        std::lock_guard lock(shard.mutex);
        for (const ManagedObject* object = shard.head; object != nullptr; object = object->next_) {
            objects.push_back(LiveObjectInfo{object->type_->name, object->id_, object->owner_,
                                             object->where_, object->time_});
        }
    }
    return objects;
}

std::vector<TypeStats> LifetimeRegistry::typeStats() const {
    std::lock_guard lock(types_mutex_);
    std::vector<TypeStats> stats;
    stats.reserve(types_.size());
    for (const auto& record : types_) {
        stats.push_back(TypeStats{record->name, record->created.load(std::memory_order_relaxed),
                                  record->live.load(std::memory_order_relaxed)});
    }
    return stats;
}

ManagedObject::ManagedObject(LifetimeToken&& token) noexcept
    : type_(token.type_),
      id_(token.id_),
      owner_(token.owner_),
      where_(token.where_),
      time_(token.time_) {
    assert(type_ != nullptr && "a LifetimeToken may be used only once");
    token.type_ = nullptr;
    LifetimeRegistry::instance().link(*this);
}

ManagedObject::~ManagedObject() {
    LifetimeRegistry::instance().unlink(*this);
}

}  // namespace dvv
