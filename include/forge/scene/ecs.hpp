#pragma once
// ECS: sparse-set storage, cache-friendly iteration, no dependencies.
// Entity = generational id (safe stale handles). Components are plain structs.

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <cassert>

namespace forge {

using Entity = uint32_t;
inline constexpr Entity kNullEntity = 0;

inline uint32_t entityIndex(Entity e) { return e & 0x00FFFFFFu; }
inline uint32_t entityGen(Entity e) { return (e >> 24) & 0xFFu; }
inline Entity makeEntity(uint32_t idx, uint32_t gen) {
    return (Entity)((gen << 24) | (idx & 0x00FFFFFFu));
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
        Entity moved = dense_.back();
        dense_[denseIdx] = moved;
        data_[denseIdx] = std::move(data_.back());
        dense_.pop_back();
        data_.pop_back();
        sparse_[entityIndex(moved)] = denseIdx;
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
        else { idx = nextIdx_++; alive_.push_back(0); }
        uint32_t gen = alive_[idx];
        return makeEntity(idx, gen);
    }

    void destroy(Entity e) {
        uint32_t i = entityIndex(e);
        if (i >= alive_.size() || alive_[i] != entityGen(e)) return; // stale
        for (auto& [ti, pool] : pools_) pool->clearFor(e);
        alive_[i] = (alive_[i] + 1) & 0xFFu;
        if (alive_[i] == 0) alive_[i] = 1; // skip gen 0 reuse confusion... 0 allowed; keep simple
        free_.push_back(i);
    }

    bool alive(Entity e) const {
        uint32_t i = entityIndex(e);
        return i < alive_.size() && alive_[i] == entityGen(e);
    }

    template <typename T, typename... Args>
    T& emplace(Entity e, Args&&... args) {
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

    // View: iterate entities having all of Ts. Callback (Entity, Ts&...).
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

    size_t entityCount() const { return nextIdx_ - free_.size(); }

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
    std::vector<uint8_t> alive_; // generation per index
    std::vector<uint32_t> free_;
    uint32_t nextIdx_ = 0;
};

} // namespace forge
