#pragma once

// 产线运行平台的命令：上线包、工位、方案包、模型（只看）、让工位持续运行。
// vision-line 只有这些；vision-cli（方案构建平台）在这些之外还有方案编排、助手、数据、训练的命令。

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "line_platform.h"

namespace vision::cli {

// 显示一次检测的结果项（缺陷、测量值、位姿）。缺陷很多时只列前几个。
void printResultItems(const ResultItems& result);

// 报告命令失败：写日志、在控制台显示，返回退出码 1。
int fail(const Error& e);

// 把命令行里给出的图像文件和目录展开成图像文件的列表（目录里的按文件名排序）。
std::vector<std::filesystem::path> expandImages(const std::vector<std::string>& args);

// release ...、station ...、package ... 三组命令。args[0] 是命令组的名字。
int runReleaseCommand(platform::LinePlatform& app, const std::vector<std::string>& args);
int runStationCommand(platform::LinePlatform& app, const std::vector<std::string>& args);
int runPackageCommand(platform::LinePlatform& app, const std::vector<std::string>& args);

// model list | show：只看，不改（产线运行平台上的模型只能由方案包带来）
int runModelViewCommand(platform::LinePlatform& app, const std::vector<std::string>& args);

// serve：让工位持续运行——每个工位一个线程，从它的图像源取图、检测，直到收到停止的请求。
//   stationIds 为空表示所有绑定了图像源的工位；exitWhenIdleSeconds > 0 时，所有工位都这么久没有新图像就自己结束（测试、演示用）。
//   stopRequested：别的线程（信号处理）把它置为真，循环就结束。
int serveStations(platform::LinePlatform& app, std::vector<std::string> stationIds, int exitWhenIdleSeconds, const std::atomic<bool>& stopRequested);

}  // namespace vision::cli
