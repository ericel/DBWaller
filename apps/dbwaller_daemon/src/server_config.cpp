#include "dbwaller/daemon/server_config.hpp"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <charconv>
#include <fstream>
#include <memory>

namespace dbwaller::daemon {
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid() { throw std::invalid_argument("invalid daemon configuration"); }
void fields(const Json& object, std::initializer_list<const char*> allowed) {
    if (!object.is_object()) invalid();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end()) invalid();
    }
}
template<class T> void number(const Json& j, const char* key, T& target) {
    if (!j.contains(key)) return;
    if (!j.at(key).is_number_unsigned()) invalid();
    const auto value = j.at(key).get<uint64_t>();
    if (value > static_cast<uint64_t>(std::numeric_limits<T>::max())) invalid();
    target = static_cast<T>(value);
}
std::filesystem::path file_at(const Json& j, const char* key, const std::filesystem::path& base) {
    auto p = std::filesystem::path(j.at(key).get<std::string>());
    if (p.empty()) invalid();
    return p.is_absolute() ? p : base / p;
}
unsigned char hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    invalid();
}
void validate_pem(const ServerConfig& cfg) {
    using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
    Bio cert_bio(BIO_new_mem_buf(cfg.certificate.data(), static_cast<int>(cfg.certificate.size())), BIO_free);
    Bio key_bio(BIO_new_mem_buf(cfg.private_key.data(), static_cast<int>(cfg.private_key.size())), BIO_free);
    if (!cert_bio || !key_bio) invalid();
    std::unique_ptr<X509, decltype(&X509_free)> cert(PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr), X509_free);
    // Never prompt on stdin for an encrypted key; unattended startup must fail.
    const auto no_password = +[](char*, int, int, void*) { return 0; };
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        PEM_read_bio_PrivateKey(key_bio.get(), nullptr, no_password, nullptr), EVP_PKEY_free);
    if (!cert || !key || X509_check_private_key(cert.get(), key.get()) != 1) invalid();
}
} // namespace

std::string read_bounded_file(const std::filesystem::path& path, size_t max_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("cannot read daemon configuration file");
    std::string result(max_bytes + 1, '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    const auto count = static_cast<size_t>(input.gcount());
    if (input.bad() || count > max_bytes) invalid();
    result.resize(count);
    return result;
}
std::array<unsigned char, 32> token_digest(const std::string& token) {
    std::array<unsigned char, 32> digest;
    if (!SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), digest.data()))
        throw std::runtime_error("credential digest unavailable");
    return digest;
}
void validate_server_config(const ServerConfig& cfg) {
    const auto colon = cfg.address.rfind(':');
    if (colon == std::string::npos) invalid();
    const auto host = cfg.address.substr(0, colon);
    unsigned port = 0;
    const auto text = cfg.address.substr(colon + 1);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
    if (host.empty() || text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || port > 65535 || cfg.address.size() > 256) invalid();
    if (cfg.insecure_loopback) {
        if ((host != "127.0.0.1" && host != "[::1]") || !cfg.certificate.empty() || !cfg.private_key.empty()) invalid();
    } else {
        if (cfg.certificate.empty() || cfg.private_key.empty() ||
            cfg.certificate.size() > 65536 || cfg.private_key.size() > 65536) invalid();
        validate_pem(cfg);
    }
    if (cfg.credentials.empty() || cfg.credentials.size() > 128 || !cfg.workers || cfg.workers > 64 ||
        !cfg.queue_capacity || cfg.queue_capacity > 4096 || !cfg.max_active_rpcs || cfg.max_active_rpcs > 8192 ||
        cfg.max_connections < 1 || cfg.max_connections > 4096 || cfg.max_streams_per_connection < 1 ||
        cfg.max_streams_per_connection > 1024 || cfg.max_message_bytes < 4096 || cfg.max_message_bytes > 16 * 1024 * 1024 ||
        cfg.transport_memory_bytes < 1024 * 1024 || cfg.transport_memory_bytes > 1024ull * 1024 * 1024 ||
        cfg.shutdown_grace_ms > 60'000 || !cfg.cache.num_shards || cfg.cache.num_shards > 256 ||
        cfg.cache.max_entries_total < cfg.cache.num_shards || cfg.cache.max_entries_total > 1'000'000 ||
        cfg.cache.max_bytes_total < cfg.cache.num_shards || cfg.cache.max_bytes_total > 1024ull * 1024 * 1024 * 1024 ||
        cfg.cache.sweep_interval_ms < 10 || cfg.cache.sweep_interval_ms > 60'000 ||
        cfg.limits.max_value_bytes > static_cast<size_t>(cfg.max_message_bytes) / 2) invalid();
    std::set<std::string> principals;
    std::set<std::array<unsigned char, 32>> hashes;
    for (const auto& c : cfg.credentials) {
        if (c.principal.empty() || c.principal.size() > cfg.limits.max_identity_bytes ||
            !principals.insert(c.principal).second || !hashes.insert(c.token_hash).second ||
            c.namespaces.size() > 128 || (c.namespaces.empty() && !c.admin)) invalid();
        for (const auto& ns : c.namespaces)
            if (ns.empty() || ns.size() > cfg.limits.max_identity_bytes) invalid();
    }
}
ServerConfig load_server_config(const std::filesystem::path& path) {
    try {
        const auto j = Json::parse(read_bounded_file(path, 65536));
        fields(j, {"listen", "insecure_loopback", "certificate_file", "private_key_file", "credentials_file",
                   "cache", "tickets", "rpc"});
        ServerConfig c;
        c.address = j.value("listen", c.address);
        c.insecure_loopback = j.value("insecure_loopback", false);
        if (j.contains("certificate_file")) c.certificate = read_bounded_file(file_at(j, "certificate_file", path.parent_path()), 65536);
        if (j.contains("private_key_file")) c.private_key = read_bounded_file(file_at(j, "private_key_file", path.parent_path()), 65536);
        const auto creds = Json::parse(read_bounded_file(file_at(j, "credentials_file", path.parent_path()), 65536));
        fields(creds, {"principals"});
        if (!creds.at("principals").is_array()) invalid();
        for (const auto& p : creds.at("principals")) {
            fields(p, {"name", "token_sha256", "namespaces", "write", "invalidate", "admin"});
            Credential entry;
            entry.principal = p.at("name").get<std::string>();
            const auto digest = p.at("token_sha256").get<std::string>();
            if (digest.size() != 64) invalid();
            for (size_t i = 0; i < 32; ++i) entry.token_hash[i] = (hex(digest[2 * i]) << 4) | hex(digest[2 * i + 1]);
            entry.namespaces = p.at("namespaces").get<std::set<std::string>>();
            entry.write = p.value("write", false);
            entry.invalidate = p.value("invalidate", false);
            entry.admin = p.value("admin", false);
            c.credentials.push_back(std::move(entry));
        }
        if (j.contains("cache")) {
            const auto& v = j.at("cache");
            fields(v, {"shards", "value_bytes", "entries", "sweep_ms", "max_key_bytes", "max_value_bytes", "max_tags", "max_tag_bytes", "max_lifetime_ms"});
            number(v, "shards", c.cache.num_shards); number(v, "value_bytes", c.cache.max_bytes_total);
            number(v, "entries", c.cache.max_entries_total); number(v, "sweep_ms", c.cache.sweep_interval_ms);
            number(v, "max_key_bytes", c.limits.max_key_bytes); number(v, "max_value_bytes", c.limits.max_value_bytes);
            number(v, "max_tags", c.limits.max_tags); number(v, "max_tag_bytes", c.limits.max_tag_bytes);
            number(v, "max_lifetime_ms", c.limits.max_cache_lifetime_ms);
        }
        if (j.contains("tickets")) {
            const auto& v = j.at("tickets");
            fields(v, {"records", "key_bytes", "lifetime_ms", "reap_per_call"});
            number(v, "records", c.limits.max_ticket_records); number(v, "key_bytes", c.limits.max_ticket_key_bytes);
            number(v, "lifetime_ms", c.limits.ticket_lifetime_ms); number(v, "reap_per_call", c.limits.reap_per_call);
        }
        if (j.contains("rpc")) {
            const auto& v = j.at("rpc");
            fields(v, {"workers", "queue", "active", "connections", "streams_per_connection", "message_bytes", "transport_memory_bytes", "shutdown_grace_ms"});
            number(v, "workers", c.workers); number(v, "queue", c.queue_capacity); number(v, "active", c.max_active_rpcs);
            number(v, "connections", c.max_connections); number(v, "streams_per_connection", c.max_streams_per_connection);
            number(v, "message_bytes", c.max_message_bytes); number(v, "transport_memory_bytes", c.transport_memory_bytes);
            number(v, "shutdown_grace_ms", c.shutdown_grace_ms);
        }
        validate_server_config(c);
        // Validate facade limits too, without starting threads or binding a port.
        if (!c.limits.max_ticket_records || !c.limits.max_ticket_key_bytes || !c.limits.reap_per_call ||
            !c.limits.ticket_lifetime_ms || c.limits.ticket_lifetime_ms > 86'400'000 || !c.limits.max_key_bytes ||
            c.limits.max_key_bytes > 1024 * 1024 || !c.limits.max_value_bytes || c.limits.max_tags > 1024 ||
            !c.limits.max_tag_bytes || c.limits.max_tag_bytes > 1024 * 1024 ||
            !c.limits.max_cache_lifetime_ms || c.limits.max_cache_lifetime_ms > 86'400'000) invalid();
        return c;
    } catch (...) {
        // JSON/PEM parser details can contain credential material. Do not echo them.
        throw std::invalid_argument("invalid daemon configuration or unreadable credential/TLS file");
    }
}
} // namespace dbwaller::daemon
