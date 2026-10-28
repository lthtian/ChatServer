// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/27 09:44
#include "media_description.h"
#include "media_repository.hpp"
#include <set>

namespace chat::media {
using json = nlohmann::json;
namespace {
constexpr std::size_t kDescriptionLimit = 65536;
void Validate(const json& value, const std::string& hash) {
    static const std::set<std::string> states{"pending", "recognized", "not_video", "unrecognized", "failed"};
    if (value.at("schema") != 1 || value.at("sha256") != hash ||
        !states.contains(value.at("state").get<std::string>())) throw MediaError("invalid_description");
    if (value.at("state") != "recognized") return;
    static const std::set<std::string> demuxers{"mov", "matroska", "webm", "avi", "mpegts", "flv", "ogg"};
    if (!demuxers.contains(value.at("demuxer").get<std::string>()) || !value.at("tracks").is_array() ||
        value.at("tracks").size() > 64 || !value.at("video_stream").is_number_integer())
        throw MediaError("invalid_description");
    for (const auto& track : value.at("tracks")) {
        if (track.at("index") == value.at("video_stream") && track.at("type") == "video" &&
            track.at("attached_picture") == false && track.at("width").is_number_integer() &&
            track.at("height").is_number_integer() && track.at("width").get<int>() > 0 &&
            track.at("height").get<int>() > 0) return;
    }
    throw MediaError("invalid_description");
}
}

json LoadDescription(const DiskStore& store, const std::string& id, const std::string& hash) {
    try {
        if (!std::filesystem::exists(store.Path(id + "/description")))
            return {{"state", "missing"}, {"failure", "media_description_missing"}};
        if (store.Size(id + "/description") > kDescriptionLimit) throw MediaError("invalid_description");
        auto file = store.Open(id + "/description");
        auto value = json::parse(file);
        Validate(value, hash);
        return value;
    } catch (const std::exception&) {
        return {{"state", "failed"}, {"failure", "media_description_invalid"}};
    }
}

void SaveDescription(const DiskStore& store, const std::string& id, const std::string& hash, json value) {
    value["schema"] = 1;
    value["sha256"] = hash;
    Validate(value, hash);
    const auto text = value.dump();
    if (text.size() > kDescriptionLimit) throw MediaError("invalid_description");
    store.Replace(id + "/description", std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

bool NeedsHlsJob(const json& description) {
    return description.value("state", "") == "recognized" && description.value("demuxer", "") == "mov";
}
}
