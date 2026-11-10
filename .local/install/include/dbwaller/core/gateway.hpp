#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <algorithm>

#include "dbwaller/adapters/adapter.hpp"
#include "dbwaller/core/sharded_engine.hpp"
#include "dbwaller/policy/rules.hpp"

namespace dbwaller::core {

// Library-level Metadata
enum class OpResult {
    Hit,
    Miss,
    StaleHit,
    Bypass,
    Error
};

struct OpMeta {
    OpResult result = OpResult::Error;
    bool refresh_triggered = false;
    std::string cache_key;
    uint64_t ttl_ms = 0;
    uint64_t swr_ms = 0;
};

class Gateway {
public:
    explicit Gateway(ShardedEngine& e) : engine_(e) {}

    // Now accepts:
    //  - optional OpMeta*
    //  - optional extra_tags (caller-provided grouping tags, e.g. timeline:home:user:<id>)
    std::optional<std::string> get_or_fetch_object_ruled(
        const dbwaller::policy::PolicyRuleSet& rules,
        std::string_view ns,
        std::string_view id,
        std::string_view op,
        const dbwaller::adapters::RequestContext& ctx,
        std::shared_ptr<dbwaller::adapters::Adapter> adapter,
        OpMeta* out_meta = nullptr,
        const std::vector<std::string>& extra_tags = {} // ✅ NEW
    ) {
        // Decide policy
        auto d = rules.decide(ns, id, op, ctx);

        // Merge policy tags + caller tags once (used for miss + refresh)
        const auto merged_tags = merge_tags_(d.tags, extra_tags);

        // Fill static meta info
        if (out_meta) {
            out_meta->cache_key = d.cache_key;
            out_meta->ttl_ms = d.ttl_ms;
            out_meta->swr_ms = d.swr_ms;
        }

        // NoStore: bypass cache
        if (!d.cacheable) {
            if (out_meta) out_meta->result = OpResult::Bypass;

            const std::string raw_key = std::string(ns) + ":" + std::string(id);
            auto r = adapter->fetch_one(raw_key, ctx);
            if (!r) return std::nullopt;
            return r->value;
        }

        // Fresh/Stale/Miss path
        auto gr = engine_.get_swr(d.cache_key);

        if (gr.kind == ShardedEngine::GetKind::Fresh && gr.value) {
            if (out_meta) out_meta->result = OpResult::Hit;
            return gr.value;
        }

        // Stale: return stale immediately, but trigger refresh in background
        if (gr.kind == ShardedEngine::GetKind::Stale && gr.value) {
            if (out_meta) {
                out_meta->result = OpResult::StaleHit;
                out_meta->refresh_triggered = (d.swr_ms > 0);
            }

            if (d.swr_ms > 0) {
                const std::string ns_s(ns);
                const std::string id_s(id);
                const std::string cache_key_s(d.cache_key);
                const auto ctx_copy = ctx;
                auto adapter_ptr = adapter;

                // Capture what we need by value.
                // NOTE: merged_tags is a local variable; capture it by value too.
                engine_.trigger_refresh(
                    cache_key_s,
                    [ns_s, id_s, ctx_copy, adapter_ptr, d, merged_tags]() -> std::pair<std::string, PutOptions> {
                        const std::string raw_key = ns_s + ":" + id_s;
                        auto r = adapter_ptr->fetch_one(raw_key, ctx_copy);
                        if (!r) {
                            throw std::runtime_error("adapter miss during refresh");
                        }

                        PutOptions opt;
                        opt.ttl_ms = d.ttl_ms;
                        opt.swr_ms = d.swr_ms;
                        opt.stale_ttl_ms = d.stale_ttl_ms;
                        opt.serve_stale_on_error = d.serve_stale_on_error;

                        // ✅ store merged tags so invalidation by caller tag works
                        opt.tags = merged_tags;

                        return {r->value, opt};
                    }
                );
            }

            return gr.value;
        }

        // Miss: blocking fill
        if (out_meta) out_meta->result = OpResult::Miss;

        const std::string filled = engine_.get_or_compute(
            d.cache_key,
            [&]() -> std::pair<std::string, PutOptions> {
                const std::string raw_key = std::string(ns) + ":" + std::string(id);
                auto r = adapter->fetch_one(raw_key, ctx);
                if (!r) throw std::runtime_error("adapter miss");

                PutOptions opt;
                opt.ttl_ms = d.ttl_ms;
                opt.swr_ms = d.swr_ms;
                opt.stale_ttl_ms = d.stale_ttl_ms;
                opt.serve_stale_on_error = d.serve_stale_on_error;

                // ✅ store merged tags so invalidation by caller tag works
                opt.tags = merged_tags;

                return {r->value, opt};
            }
        );

        if (filled.empty()) return std::nullopt;
        return filled;
    }

private:
    ShardedEngine& engine_;

    static std::vector<std::string> merge_tags_(
        const std::vector<std::string>& base,
        const std::vector<std::string>& extra
    ) {
        if (extra.empty()) {
            return base; // copy
        }

        std::vector<std::string> out;
        out.reserve(base.size() + extra.size());
        out.insert(out.end(), base.begin(), base.end());
        out.insert(out.end(), extra.begin(), extra.end());

        // Optional dedupe. Helps avoid exploding tag lists if caller repeats.
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
};

} // namespace dbwaller::core





// #pragma once

// #include <optional>
// #include <string>
// #include <string_view>
// #include <utility>

// #include "dbwaller/adapters/adapter.hpp"
// #include "dbwaller/core/sharded_engine.hpp"
// #include "dbwaller/policy/rules.hpp"

// namespace dbwaller::core {

// // Library-level Metadata
// enum class OpResult {
//     Hit,
//     Miss,
//     StaleHit,
//     Bypass,
//     Error
// };

// struct OpMeta {
//     OpResult result = OpResult::Error;
//     bool refresh_triggered = false;
//     std::string cache_key;
//     uint64_t ttl_ms = 0;
//     uint64_t swr_ms = 0;
// };

// class Gateway {
// public:
//     explicit Gateway(ShardedEngine& e) : engine_(e) {}

//     // Now accepts an optional pointer to OpMeta
//     std::optional<std::string> get_or_fetch_object_ruled(
//         const dbwaller::policy::PolicyRuleSet& rules,
//         std::string_view ns,
//         std::string_view id,
//         std::string_view op,
//         const dbwaller::adapters::RequestContext& ctx,
//         //dbwaller::adapters::Adapter& adapter,
//         std::shared_ptr<dbwaller::adapters::Adapter> adapter,
//         OpMeta* out_meta = nullptr
//     ) {
//         // Decide policy
//         auto d = rules.decide(ns, id, op, ctx);

//         // Fill static meta info
//         if (out_meta) {
//             out_meta->cache_key = d.cache_key;
//             out_meta->ttl_ms = d.ttl_ms;
//             out_meta->swr_ms = d.swr_ms;
//         }

//         // NoStore: bypass cache
//         if (!d.cacheable) {
//             if (out_meta) out_meta->result = OpResult::Bypass;

//             const std::string raw_key = std::string(ns) + ":" + std::string(id);
//             auto r = adapter->fetch_one(raw_key, ctx);
//             if (!r) return std::nullopt;
//             return r->value;
//         }

//         // Fresh/Stale/Miss path
//         auto gr = engine_.get_swr(d.cache_key);

//         if (gr.kind == ShardedEngine::GetKind::Fresh && gr.value) {
//             if (out_meta) out_meta->result = OpResult::Hit;
//             return gr.value;
//         }

//         // Stale: return stale immediately, but trigger refresh in background
//         if (gr.kind == ShardedEngine::GetKind::Stale && gr.value) {
//             if (out_meta) {
//                 out_meta->result = OpResult::StaleHit;
//                 // Only mark as triggered if we actually schedule it below (swr > 0)
//                 out_meta->refresh_triggered = (d.swr_ms > 0);
//             }

//             if (d.swr_ms > 0) {
//                 const std::string ns_s(ns);
//                 const std::string id_s(id);
//                 const std::string cache_key_s(d.cache_key);
//                 const auto ctx_copy = ctx; 
//                 //auto& adapter_ref = adapter;
//                 auto adapter_ptr = adapter;


//                 engine_.trigger_refresh(cache_key_s, [ns_s, id_s, ctx_copy, adapter_ptr, d]() -> std::pair<std::string, PutOptions> {
//                     const std::string raw_key = ns_s + ":" + id_s;
//                     auto r = adapter_ptr->fetch_one(raw_key, ctx_copy);
//                     if (!r) {
//                         throw std::runtime_error("adapter miss during refresh");
//                     }

//                     PutOptions opt;
//                     opt.ttl_ms = d.ttl_ms;
//                     opt.swr_ms = d.swr_ms;
//                     opt.stale_ttl_ms = d.stale_ttl_ms;
//                     opt.serve_stale_on_error = d.serve_stale_on_error;
//                     opt.tags = d.tags;

//                     return {r->value, opt};
//                 });
//             }

//             return gr.value;
//         }

//         // Miss: blocking fill
//         if (out_meta) out_meta->result = OpResult::Miss;

//         const std::string filled = engine_.get_or_compute(d.cache_key, [&]() -> std::pair<std::string, PutOptions> {
//             const std::string raw_key = std::string(ns) + ":" + std::string(id);
//             auto r = adapter->fetch_one(raw_key, ctx);
//             if (!r) throw std::runtime_error("adapter miss");

//             PutOptions opt;
//             opt.ttl_ms = d.ttl_ms;
//             opt.swr_ms = d.swr_ms;
//             opt.stale_ttl_ms = d.stale_ttl_ms;
//             opt.serve_stale_on_error = d.serve_stale_on_error;
//             opt.tags = d.tags;

//             return {r->value, opt};
//         });

//         if (filled.empty()) return std::nullopt;
//         return filled;
//     }

// private:
//     ShardedEngine& engine_;
// };

// } // namespace dbwaller::core

