// 这个文件【故意】违反上下文边界：方案编排的应用层代码去包含检测执行的领域层头文件。
// 它不参与正常构建（EXCLUDE_FROM_ALL）。测试 boundary_violation_fails_to_compile 会尝试构建它，
// 并期望构建失败——证明边界由构建系统强制，而不是靠人自觉遵守（ADR-17）。

#include "vision/authoring/api.h"
#include "vision/execution/domain/inspection.h"  // ← 违规：不允许跨上下文引用领域类

int boundaryViolation() {
    auto i = vision::execution::Inspection::start("x", vision::execution::InspectionPurpose::Debug, "v", {});
    return static_cast<int>(i.completed());
}
