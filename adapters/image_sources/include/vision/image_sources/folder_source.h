#pragma once

#include <filesystem>
#include <set>
#include <string>

#include "vision/station/api.h"

namespace vision::image_sources {

// 图像源的一种实现：一个目录里的图像文件（种类 "folder"）。
//
// 每次取图时重新看一遍目录，把还没有处理过的图像按文件名的顺序交出去，每个文件只交一次。
// 所以它有两种用法：
//   - 目录里事先放好一批图像：一张张处理完，之后“没有新的”
//   - 一边运行一边往目录里放新图像：新放进来的会被接着处理（一个最简单的“假相机”）
//
// “处理过哪些”只记在内存里：工位停止再启动（图像源重新打开），目录里的图像会从头再来一遍。
// 只认 .png、.jpg、.jpeg、.bmp；别的文件和子目录不理会。
class FolderImageSource final : public station::ImageSource {
public:
    explicit FolderImageSource(std::filesystem::path directory) : directory_(std::move(directory)) {}
    Result<std::optional<station::Frame>> next() override;

private:
    std::filesystem::path directory_;
    std::set<std::string> seen_;  // 已经交出去过的文件名（包括读不出来的：不会卡在同一个坏文件上）
};

// 图像源工厂：按种类打开图像源。现在只认识 "folder"。
class ImageSources final : public station::ImageSourceFactory {
public:
    Result<std::unique_ptr<station::ImageSource>> open(const std::string& kind, const std::string& location) override;
};

}  // namespace vision::image_sources
