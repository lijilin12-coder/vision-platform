#pragma once

// vision-cli 各个源文件共用的部分：组装好的全部服务，以及几个小工具。

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "platform.h"

namespace vision::cli {

// 组装好的整个平台（和 vision-runtime 共用，见 apps/common/platform.h）
using App = platform::Platform;

// 显示一次检测的结果项（缺陷、测量值、位姿）。缺陷很多时只列前几个。
void printResultItems(const ResultItems& result);

// 报告命令失败：写日志、在控制台显示，返回退出码 1。
int fail(const Error& e);

// 把命令行里给出的图像文件和目录展开成图像文件的列表（目录里的按文件名排序）。
std::vector<std::filesystem::path> expandImages(const std::vector<std::string>& args);

// release ... 和 station ... 两组命令。args[0] 是 "release" 或 "station"。
int runReleaseCommand(App& app, const std::vector<std::string>& args);
int runStationCommand(App& app, const std::vector<std::string>& args);

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
