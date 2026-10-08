#include "hls_catalog.hpp"
#include "media_repository.hpp"
#include <algorithm>
#include <regex>
#include <unordered_set>

namespace chat::media {
namespace {
nlohmann::json ReadJson(const DiskStore& store, const std::string& key, std::size_t limit) {
    try {
        auto file = store.Open(key);
        // 同一个文件句柄读取，避免目录切换期间长度和内容属于不同版本。
        file.seekg(0, std::ios::end);
        const auto length = file.tellg();
        if (length <= 0 || length > static_cast<std::streamoff>(limit)) throw MediaError("invalid_catalog");
        file.seekg(0);
        std::string text(static_cast<std::size_t>(length), '\0');
        if (!file.read(text.data(), length)) throw MediaError("invalid_catalog");
        return nlohmann::json::parse(text);
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory) return nullptr;
        throw;
    }
}
}

std::shared_ptr<const HlsCatalog> LoadHls(const DiskStore& store, const std::string& id,
                                        const std::string& sha256) {
    const auto data = ReadJson(store, id + "/hls/catalog", 512 * 1024);
    if (data.is_null()) return {};
    return ParseHls(data, id, sha256);
}

std::shared_ptr<const HlsCatalog> ParseHls(const nlohmann::json& data, const std::string& id,
                                         const std::string& sha256) {
    static const std::regex revision_pattern("[0-9a-f]{32}");
    static const std::regex asset_pattern("master\\.m3u8|[0-9]{1,4}p/(index\\.m3u8|init\\.mp4|seg_[0-9]{5}\\.(ts|m4s))");
    if (data.at("version") != 1 || data.at("source_sha256") != sha256 ||
        !std::regex_match(data.at("revision").get<std::string>(), revision_pattern) ||
        !data.at("assets").is_array() || data["assets"].empty() || data["assets"].size() > 4096 ||
        !data.at("variants").is_array() || data["variants"].empty() || data["variants"].size() > 3)
        throw MediaError("invalid_catalog");
    auto catalog = std::make_shared<HlsCatalog>();
    catalog->revision = data["revision"];
    std::uint64_t total = 0;
    for (const auto& item : data["assets"]) {
        const auto path = item.at("path").get<std::string>();
        const auto bytes = item.at("bytes").get<std::int64_t>();
        if (!std::regex_match(path, asset_pattern) || bytes <= 0 || bytes > 256 * 1024 * 1024)
            throw MediaError("invalid_catalog");
        total += bytes;
        if (total > 256 * 1024 * 1024) throw MediaError("invalid_catalog");
        auto key = path;
        std::replace(key.begin(), key.end(), '.', '_');
        const auto mime = path.ends_with(".m3u8") ? "application/vnd.apple.mpegurl" :
                          path.ends_with(".ts") ? "video/mp2t" : "video/mp4";
        if (!catalog->assets.emplace(path, HlsAsset{id + "/hls/" + catalog->revision + "/" + key,
                                                  mime, static_cast<std::uint64_t>(bytes)}).second)
            throw MediaError("invalid_catalog");
    }
    if (!catalog->assets.contains("master.m3u8")) throw MediaError("invalid_catalog");
    int last_height = 10000;
    catalog->variants = nlohmann::json::array();
    for (const auto& variant : data["variants"]) {
        const int height = variant.at("height"), width = variant.at("width");
        const auto name = variant.at("id").get<std::string>();
        const auto playlist = variant.at("playlist").get<std::string>();
        if (height < 2 || height >= last_height || height > data.at("source_height").get<int>() ||
            width < 2 || width > 16384 || name != std::to_string(height) + "p" ||
            playlist != name + "/index.m3u8" || !catalog->assets.contains(playlist))
            throw MediaError("invalid_catalog");
        last_height = height;
        catalog->variants.push_back({{"id", name}, {"height", height}, {"width", width},
            {"playlist", playlist}, {"bandwidth", variant.at("bandwidth")}});
    }
    return catalog;
}

std::string HlsState(const DiskStore& store, const std::string& id) {
    const auto value = ReadJson(store, id + "/hls/status", 1024);
    const auto state = value.is_object() ? value.value("state", "pending") : "pending";
    return state == "processing" || state == "failed" ? state : "pending";
}
}
