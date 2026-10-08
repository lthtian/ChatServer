#pragma once
#include "chat/media/disk_store.h"
#include "json.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace chat::media {
struct HlsAsset {
    std::string key;  // 服务器内部对象键，客户端不能直接指定。
    std::string mime;  // 列表、TS、MP4 的响应类型。
    std::uint64_t bytes = 0;  // 已发布对象长度，供 Range 和截断检查使用。
};
struct HlsCatalog {
    std::string revision;  // 不可变的一组产物版本，绑定整套读取凭证。
    nlohmann::json variants;  // 已就绪档位和相对播放列表路径。
    std::unordered_map<std::string, HlsAsset> assets;  // 允许通过 HTTP 读取的相对路径。
};
// 校验待发布清单，生成与 HTTP 鉴权相同的对象白名单。
std::shared_ptr<const HlsCatalog> ParseHls(const nlohmann::json& data, const std::string& media_id,
                                         const std::string& source_sha256);
// 在磁盘工作线程读取有上限的清单；缺失返回空，损坏则报错，不公开半成品。
std::shared_ptr<const HlsCatalog> LoadHls(const DiskStore& store, const std::string& media_id,
                                        const std::string& source_sha256);
// 没有可用清单时返回 pending/processing/failed；成功是否就绪以清单为准。
std::string HlsState(const DiskStore& store, const std::string& media_id);
}
