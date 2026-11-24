#include "vision/image_sources/folder_source.h"

#include <algorithm>
#include <vector>

#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::image_sources {

namespace fs = std::filesystem;

namespace {

bool isImageFile(const fs::path& path) {
    std::string ext = pathToUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
}

}  // namespace

Result<std::optional<station::Frame>> FolderImageSource::next() {
    using R = Result<std::optional<station::Frame>>;
    std::error_code ec;
    std::vector<std::string> waiting;
    for (fs::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !isImageFile(it->path())) continue;
        std::string name = pathToUtf8(it->path().filename());
        if (seen_.count(name) == 0) waiting.push_back(std::move(name));
    }
    if (ec) return R::failure("image_source.unreadable", "读不了图像目录 " + pathToUtf8(directory_) + "：" + ec.message());
    if (waiting.empty()) return std::optional<station::Frame>{};

    // 按文件名的顺序：同一批图像每次处理的顺序都一样
    const std::string name = *std::min_element(waiting.begin(), waiting.end());
    seen_.insert(name);  // 先记下再读：读不出来的文件也算处理过了，下一次接着取后面的
    auto image = imaging::loadImage(pathToUtf8(directory_ / pathFromUtf8(name)));
    if (!image) return R::failure("image_source.bad_frame", "图像 " + name + " 读不出来：" + image.error().message);
    return std::optional<station::Frame>{station::Frame{std::move(image).value(), name}};
}

Result<std::unique_ptr<station::ImageSource>> ImageSources::open(const std::string& kind, const std::string& location) {
    using R = Result<std::unique_ptr<station::ImageSource>>;
    if (kind != "folder") return R::failure("image_source.unknown_kind", "不认识的图像源种类：" + kind + "（现在只有 folder：一个目录里的图像）");
    const fs::path directory = pathFromUtf8(location);
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return R::failure("image_source.not_found", "图像目录不存在：" + location);
    return std::unique_ptr<station::ImageSource>(std::make_unique<FolderImageSource>(directory));
}

}  // namespace vision::image_sources
