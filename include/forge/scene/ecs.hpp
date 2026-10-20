#pragma once
// ECS: sparse-set storage, cache-friendly iteration, no dependencies.
// Entity = generational id (safe stale handles). Components are plain structs.

#include "forge/core/logger.hpp"
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <cassert>

namespace forge {

using Entity = uint32_t;
inline constexpr Entity kNullEntity = 0; // never issued: indices start at 1
inline constexpr uint32_t kMaxEntityIndex = 0x000FFFFFu; // 20-bit index field
inline constexpr uint32_t kMaxEntityGen = 0xFFFu;        // 12-bit generation field

inline uint32_t entityIndex(Entity e) { return e & 0x000FFFFFu; }       // 20-bit index
inline uint32_t entityGen(Entity e) { return (e >> 20) & 0xFFFu; }      // 12-bit generation
inline Entity makeEntity(uint32_t idx, uint32_t gen) {
    return (Entity)(((gen & 0xFFFu) << 20) | (idx & 0x000FFFFFu));
}

class IPool {
public:
    virtual ~IPool() = default;
    virtual void removeAt(uint32_t denseIdx) = 0;
    virtual void clearFor(Entity e) = 0;
    virtual bool hasEntity(Entity e) const = 0;
};

template <typename T>
class Pool : public IPool {
public:
    bool has(Entity e) const {
        uint32_t i = entityIndex(e);
        return i < sparse_.size() && sparse_[i] != kDenseNull && dense_[sparse_[i]] == e;
    }
    bool hasEntity(Entity e) const override { return has(e); }

    T& get(Entity e) {
        assert(has(e));
        return data_[sparse_[entityIndex(e)]];
    }
    const T& get(Entity e) const {
        assert(has(e));
        return data_[sparse_[entityIndex(e)]];
    }

    template <typename... Args>
    T& emplace(Entity e, Args&&... args) {
        uint32_t i = entityIndex(e);
        if (i >= sparse_.size()) sparse_.resize(i + 1, kDenseNull);
        if (has(e)) {
            data_[sparse_[i]] = T{std::forward<Args>(args)...};
            return data_[sparse_[i]];
        }
        uint32_t d = (uint32_t)data_.size();
        sparse_[i] = d;
        dense_.push_back(e);
        data_.emplace_back(std::forward<Args>(args)...);
        return data_.back();
    }

    void remove(Entity e) {
        uint32_t i = entityIndex(e);
        if (!has(e)) return;
        removeAt(sparse_[i]);
    }

    void removeAt(uint32_t denseIdx) override {
        assert(denseIdx < dense_.size());
        uint32_t last = (uint32_t)dense_.size() - 1;
        Entity removed = dense_[denseIdx];
        if (denseIdx != last) {
            dense_[denseIdx] = dense_[last];
            data_[denseIdx] = std::move(data_[last]);
            sparse_[entityIndex(dense_[denseIdx])] = denseIdx;
        }
        dense_.pop_back();
        data_.pop_back();
        if (entityIndex(removed) < sparse_.size())
            sparse_[entityIndex(removed)] = kDenseNull;
    }

    void clearFor(Entity e) override { remove(e); }

    // Iteration
    size_t size() const { return data_.size(); }
    T& operator[](size_t d) { return data_[d]; }
    Entity entityAt(size_t d) const { return dense_[d]; }

    void clear() { data_.clear(); dense_.clear(); sparse_.clear(); }

private:
    static constexpr uint32_t kDenseNull = 0xFFFFFFFFu;
    std::vector<uint32_t> sparse_;
    std::vector<Entity> dense_;
    std::vector<T> data_;
};

class Registry {
public:
    Registry() = default;

    Entity create() {
        uint32_t idx;
        if (!free_.empty()) { idx = free_.back(); free_.pop_back(); }
        else {
            idx = nextIdx_;
            if (idx == 0) idx = nextIdx_ = 1; // index 0 reserved: kNullEntity
            // The index field is 20 bits. Going past it would silently alias
            // an existing entity (makeEntity masks the index), so refuse --
            // loudly, but without crashing a shipping game (log + null).
            if (idx > kMaxEntityIndex) {
                FORGE_LOG_ERROR("Registry::create: entity index space exhausted "
                                "(> %u live+recycled slots); returning kNullEntity",
                                (unsigned)kMaxEntityIndex);
                return kNullEntity;
            }
            if (idx >= alive_.size()) alive_.resize(idx + 1, 0);
            nextIdx_ = idx + 1;
        }
        return makeEntity(idx, alive_[idx]);
    }

    void destroy(Entity e) {
        if (e == kNullEntity) return;
        for (auto& [ti, pool] : pools_) pool->clearFor(e);
        uint32_t i = entityIndex(e);
        // clearFor is gen-checked (no-op for stale handles), so clearing first
        // also reaps orphans; the gen check below guards double-destroy recycle.
        if (i >= alive_.size() || alive_[i] != entityGen(e)) return; // stale
        alive_[i] = (uint16_t)((alive_[i] + 1) & kMaxEntityGen);
        free_.push_back(i);
    }

    bool alive(Entity e) const {
        if (e == kNullEntity) return false;
        uint32_t i = entityIndex(e);
        return i < alive_.size() && i != 0 && alive_[i] == entityGen(e);
    }

    template <typename T, typename... Args>
    T& emplace(Entity e, Args&&... args) {
        assert(alive(e) && "Registry::emplace on dead/null entity");
        return pool<T>().emplace(e, std::forward<Args>(args)...);
    }

    template <typename T>
    void remove(Entity e) { pool<T>().remove(e); }

    template <typename T>
    bool has(Entity e) const { return pool<T>().has(e); }

    template <typename T>
    T& get(Entity e) { return pool<T>().get(e); }
    template <typename T>
    const T& get(Entity e) const { return pool<T>().get(e); }

    template <typename T>
    T* tryGet(Entity e) { auto& p = pool<T>(); return p.has(e) ? &p.get(e) : nullptr; }
    template <typename T>
    const T* tryGet(Entity e) const { const auto& p = pool<T>(); return p.has(e) ? &p.get(e) : nullptr; }

    // View: iterate entities having all of Ts. Callback (Entity, Ts&...).
    // The callback must NOT create/destroy entities or add/remove components
    // (structural ops invalidate the iteration). Defer such ops until after.
    template <typename... Ts, typename Fn>
    void view(Fn&& fn) {
        auto& first = pool<First<Ts...>>();
        for (size_t d = 0; d < first.size(); ++d) {
            Entity e = first.entityAt(d);
            if (!alive(e)) continue;
            if ((has<Ts>(e) && ...)) fn(e, get<Ts>(e)...);
        }
    }

    template <typename... Ts, typename Fn>
    void view(Fn&& fn) const {
        auto& first = pool<First<Ts...>>();
        for (size_t d = 0; d < first.size(); ++d) {
            Entity e = first.entityAt(d);
            if (!alive(e)) continue;
            if ((has<Ts>(e) && ...)) fn(e, get<Ts>(e)...);
        }
    }

    void clear() {
        pools_.clear();
        alive_.clear();
        free_.clear();
        nextIdx_ = 0;
    }

    size_t entityCount() const {
        size_t issued = nextIdx_ > 0 ? (size_t)nextIdx_ - 1 : 0; // index 0 reserved
        return issued >= free_.size() ? issued - free_.size() : 0;
    }

private:
    template <typename T, typename... Rest> struct FirstHelper { using type = T; };
    template <typename... Ts> using First = typename FirstHelper<Ts...>::type;

    template <typename T>
    Pool<T>& pool() const {
        std::type_index ti(typeid(T));
        auto it = pools_.find(ti);
        if (it == pools_.end()) {
            auto p = std::make_unique<Pool<T>>();
            Pool<T>* ptr = p.get();
            pools_.emplace(ti, std::move(p));
            return *ptr;
        }
        return *static_cast<Pool<T>*>(it->second.get());
    }

    mutable std::unordered_map<std::type_index, std::unique_ptr<IPool>> pools_;
    // Per-index generation. MUST hold the full 12-bit range: a uint8_t here
    // aliases stale handles after 256 recycles (gen & 0xFFF never wraps in
    // the handle, so a truncated store can match a dead entity).
    std::vector<uint16_t> alive_;
    std::vector<uint32_t> free_;
    uint32_t nextIdx_ = 0;
};

} // namespace forge
