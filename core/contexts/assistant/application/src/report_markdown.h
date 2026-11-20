#pragma once

#include <string>

#include "vision/assistant/domain/session.h"

namespace vision::assistant {

// 把助手报告写成给人看的 Markdown
std::string renderReport(const SessionState& state, const Workflow& workflow);

}  // namespace vision::assistant
