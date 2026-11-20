// 这个文件【故意】违反平台的划分：产线运行平台用到的库（工位运行、上线管理、检测执行）
// 去使用方案助手。它不参与正常构建（EXCLUDE_FROM_ALL）。
// 测试 boundary_runtime_cannot_use_assistant 会尝试构建它，并期望构建失败——
// 证明“产线上的程序里没有助手、没有大模型、没有任何会自己改方案的东西”是由构建系统保证的（ADR-28）。

#include "vision/station/api.h"
#include "vision/assistant/api.h"  // ← 违规：产线运行平台够不着方案助手

int runtimeUsesAssistant() {
    vision::assistant::AssistRequest request;
    return request.maxRounds;
}
