// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/27 09:44
#pragma once

#include "chat/media/disk_store.h"
#include "json.hpp"

namespace chat::media {
// 描述与原件摘要绑定；缺失不触发自动回填，损坏返回明确失败状态。
nlohmann::json LoadDescription(const DiskStore& store, const std::string& id, const std::string& hash);
void SaveDescription(const DiskStore& store, const std::string& id,
                     const std::string& hash, nlohmann::json description);
// 当前 HLS 处理器接受 MOV 家族；是否完成派生仍由处理任务决定。
bool NeedsHlsJob(const nlohmann::json& description);
}
