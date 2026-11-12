#pragma once

#include "vision/common/result.h"
#include "vision/kernel/signature.h"

namespace vision::catalog {

// 算子注册时的业务规则（事件 A2：注册失败的原因）：
//  - ID 非空，形如 "分类.名称"
//  - 主版本号 >= 1
//  - 输入、输出、参数各自不能重名
//  - 参数默认值必须满足它自己的定义
//  - Choice 参数至少有一个选项
Status validateSignature(const OperatorSignature& signature);

}  // namespace vision::catalog
