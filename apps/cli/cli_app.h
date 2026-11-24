#pragma once

// vision-cli 各个源文件共用的部分：组装好的全部服务，以及几个小工具。

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "line_commands.h"  // release、station、package 这几组命令和小工具：和 vision-line 共用
#include "platform.h"

namespace vision::cli {

// 组装好的整个平台（和 vision-runtime 共用，见 apps/common/platform.h）
using App = platform::Platform;

// golden ...、workflow ...、assist ... 三组命令（方案助手，见 assist_commands.cpp）。
int runGoldenCommand(App& app, const std::vector<std::string>& args);
int runWorkflowCommand(App& app, const std::vector<std::string>& args);
int runAssistCommand(App& app, const std::vector<std::string>& args);

// sample ...、dataset ... 两组命令（数据，见 data_commands.cpp）。
int runSampleCommand(App& app, const std::vector<std::string>& args);
int runDatasetCommand(App& app, const std::vector<std::string>& args);
// train ...、model ... 两组命令（训练任务和模型仓库，见 data_commands.cpp）。
int runTrainCommand(App& app, const std::vector<std::string>& args);
int runModelCommand(App& app, const std::vector<std::string>& args);

}  // namespace vision::cli
