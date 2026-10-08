#pragma once
#include <string>
namespace media {
// 按源尺寸筛选档位；生成 TS 或 fMP4 HLS，完成后原子改名输出目录。
void PackageHls(const std::string& input, const std::string& directory, bool fmp4);
}
