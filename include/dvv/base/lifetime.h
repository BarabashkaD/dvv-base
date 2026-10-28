#pragma once

// Factory-only creation and the lifetime registry (spec §9.4).
//
// A managed class is final, derives Managed<Self> and takes a CreationKey<Authority> as its first
// constructor parameter. Only the authority can create the key, so any other construction fails to
// compile. Every live managed object is linked into the process-wide LifetimeRegistry.

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace dvv {

using ObjectId = std::uint64_t;
inline constexpr ObjectId NO_OWNER = 0;

// One record per managed type for the whole process.
struct TypeRecord {
    std::string name;
    std::atomic<std::uint64_t> created{0};
    std::atomic<std::int64_t> live{0};
};

struct TypeStats {
    std::string_view name;
    std::uint64_t created = 0;
    std::int64_t live = 0;
};

struct LiveObjectInfo {
    std::string_view type;
    ObjectId id = 0;
    ObjectId owner = NO_OWNER;
    std::optional<std::source_location> createdAt;                     // detailed mode only
    std::optional<std::chrono::system_clock::time_point> createdTime;  // detailed mode only
};

class LifetimeRegistry;
class ManagedObject;

// Identity of one object about to be created; issued by LifetimeRegistry::issue<T>().
class LifetimeToken {
public:
    // A moved-from token is empty (type() == nullptr), so one token creates at most one object.
    LifetimeToken(LifetimeToken&& other) noexcept { takeFrom(other); }
    LifetimeToken& operator=(LifetimeToken&& other) noexcept {
        if (this != &other) {
            takeFrom(other);
        }
        return *this;
    }
    LifetimeToken(const LifetimeToken&) = delete;
    LifetimeToken& operator=(const LifetimeToken&) = delete;
    ~LifetimeToken() = default;

    [[nodiscard]] ObjectId id() const noexcept { return id_; }
    [[nodiscard]] const TypeRecord* type() const noexcept { return type_; }

private:
    friend class LifetimeRegistry;
    friend class ManagedObject;
    LifetimeToken() noexcept = default;

    void takeFrom(LifetimeToken& other) noexcept {
        type_ = std::exchange(other.type_, nullptr);
        id_ = other.id_;
        owner_ = other.owner_;
        where_ = other.where_;
        time_ = other.time_;
    }

    TypeRecord* type_ = nullptr;
    ObjectId id_ = 0;
    ObjectId owner_ = NO_OWNER;
    std::optional<std::source_location> where_;
    std::optional<std::chrono::system_clock::time_point> time_;
};

// Passkey and lifetime token in one. Only the authority F can create it.
template <class F>
class CreationKey {
public:
    CreationKey(CreationKey&&) noexcept = default;
    CreationKey& operator=(CreationKey&&) noexcept = default;
    CreationKey(const CreationKey&) = delete;
    CreationKey& operator=(const CreationKey&) = delete;
    ~CreationKey() = default;

    [[nodiscard]] LifetimeToken take() && noexcept { return std::move(token_); }

private:
    friend F;
    explicit CreationKey(LifetimeToken token) noexcept : token_(std::move(token)) {}

    LifetimeToken token_;
};

// Process infrastructure: intentionally never destroyed, so it outlives every managed object.
class LifetimeRegistry {
public:
    static LifetimeRegistry& instance() noexcept;

    LifetimeRegistry(const LifetimeRegistry&) = delete;
    LifetimeRegistry& operator=(const LifetimeRegistry&) = delete;
    LifetimeRegistry(LifetimeRegistry&&) = delete;
    LifetimeRegistry& operator=(LifetimeRegistry&&) = delete;

    // Issues the token for one new object of type T.
    template <class T>
    [[nodiscard]] LifetimeToken issue(ObjectId owner = NO_OWNER,
                                      std::source_location where = std::source_location::current());

    // Detailed mode (Debug builds or --diag) also records creation location and time.
    void setDetailed(bool detailed) noexcept;
    [[nodiscard]] bool detailed() const noexcept;

    [[nodiscard]] std::size_t liveCount() const noexcept;
    [[nodiscard]] std::vector<LiveObjectInfo> liveObjects() const;
    [[nodiscard]] std::vector<TypeStats> typeStats() const;

    // Returns the record for a type name, creating it on first use.
    [[nodiscard]] TypeRecord& typeRecord(std::string_view name);

private:
    friend class ManagedObject;
    static constexpr std::size_t SHARD_COUNT = 16;

    // Cache-line aligned so neighbouring shard mutexes do not share a line (no false sharing).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padding to the alignment is the point
#endif
    struct alignas(64) Shard {
        mutable std::mutex mutex;
        ManagedObject* head = nullptr;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    LifetimeRegistry();
    ~LifetimeRegistry() = default;

    [[nodiscard]] LifetimeToken makeToken(TypeRecord& type, ObjectId owner,
                                          std::source_location where) noexcept;
    void link(ManagedObject& object) noexcept;
    void unlink(ManagedObject& object) noexcept;

    std::array<Shard, SHARD_COUNT> shards_;
    std::atomic<ObjectId> next_id_{1};
    std::atomic<bool> detailed_{false};
    std::atomic<std::size_t> live_count_{0};
    mutable std::mutex types_mutex_;
    std::vector<std::unique_ptr<TypeRecord>> types_;
};

namespace detail {

[[nodiscard]] std::string demangle(const char* name);

template <class T>
TypeRecord& typeRecordOf() {
    static TypeRecord& record = LifetimeRegistry::instance().typeRecord(demangle(typeid(T).name()));
    return record;
}

}  // namespace detail

template <class T>
LifetimeToken LifetimeRegistry::issue(ObjectId owner, std::source_location where) {
    return makeToken(detail::typeRecordOf<T>(), owner, where);
}

// Non-template base: identity reachable through interface pointers.
class ManagedObject {
public:
    ManagedObject(const ManagedObject&) = delete;
    ManagedObject& operator=(const ManagedObject&) = delete;
    ManagedObject(ManagedObject&&) = delete;
    ManagedObject& operator=(ManagedObject&&) = delete;

    [[nodiscard]] ObjectId objectId() const noexcept { return id_; }
    [[nodiscard]] ObjectId ownerId() const noexcept { return owner_; }
    [[nodiscard]] std::string_view typeName() const noexcept { return type_->name; }

protected:
    explicit ManagedObject(LifetimeToken&& token) noexcept;
    // Unregisters: takes the shard mutex for an O(1) unlink. It can wait briefly while
    // liveObjects() copies that shard.
    ~ManagedObject();

private:
    friend class LifetimeRegistry;

    TypeRecord* type_;
    ObjectId id_;
    ObjectId owner_;
    std::optional<std::source_location> where_;
    std::optional<std::chrono::system_clock::time_point> time_;
    ManagedObject* prev_ = nullptr;
    ManagedObject* next_ = nullptr;
};

// CRTP base of every managed class.
template <class T>
class Managed : public ManagedObject {
protected:
    template <class F>
    explicit Managed(CreationKey<F>&& key) noexcept
        : ManagedObject(checked(std::move(key).take())) {}
    ~Managed() = default;

private:
    static LifetimeToken&& checked(LifetimeToken&& token) noexcept {
        assert(token.type() == &detail::typeRecordOf<T>() && "token issued for another type");
        return std::move(token);
    }
};

template <class T>
concept ManagedType = std::is_final_v<T> && std::derived_from<T, Managed<T>> &&
                      !std::is_copy_constructible_v<T> && !std::is_move_constructible_v<T>;

}  // namespace dvv
