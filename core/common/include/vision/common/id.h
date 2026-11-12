#pragma once

#include <string>

namespace vision {

// 全局唯一 ID（UUID v4）。方案、上线包要在多台机器之间传递，不能用自增数字（H-06）。
std::string newUuid();

}  // namespace vision
