// 第 1 课：读取和保存图像。读取失败不会报错；PNG 和 JPEG 的区别；路径里有中文怎么办。
#include "cvtut.h"

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    const cv::Mat part = cvtut::makePartImage();

    // ---------- 1. 保存和读取 ----------
    const std::string pngFile = (out / "01_part.png").string();
    c.check(cv::imwrite(pngFile, part), "imwrite 保存成功时返回 true");

    const cv::Mat loaded = cv::imread(pngFile, cv::IMREAD_UNCHANGED);
    c.check(!loaded.empty(), "imread 读取成功");
    std::cout << "读到的图像：" << loaded.cols << "×" << loaded.rows << "，通道数 " << loaded.channels() << "\n";

    // ---------- 2. 读取失败不会报错，只会返回一张空图像 ----------
    const cv::Mat missing = cv::imread((out / "not_exist.png").string());
    c.check(missing.empty(), "文件不存在：imread 返回空图像，不抛异常，程序照常往下走");
    // 所以每次读图之后都必须检查 empty()。不检查的话，错误会在后面某个不相干的地方才冒出来。

    // ---------- 3. 读取方式决定了通道数 ----------
    const cv::Mat asColor = cv::imread(pngFile, cv::IMREAD_COLOR);          // 总是转成 3 通道
    const cv::Mat asGray = cv::imread(pngFile, cv::IMREAD_GRAYSCALE);       // 总是转成 1 通道
    const cv::Mat asIs = cv::imread(pngFile, cv::IMREAD_UNCHANGED);         // 文件里是什么就是什么
    c.check(asColor.channels() == 3, "IMREAD_COLOR（默认）：灰度图也会被读成 3 通道");
    c.check(asGray.channels() == 1, "IMREAD_GRAYSCALE：读成 1 通道");
    c.check(asIs.channels() == 1, "IMREAD_UNCHANGED：保持文件原样（这个文件是 1 通道）");

    // ---------- 4. PNG 无损，JPEG 有损 ----------
    const cv::Mat fromPng = cv::imread(pngFile, cv::IMREAD_UNCHANGED);
    c.check(cv::norm(part, fromPng, cv::NORM_INF) == 0, "PNG：读回来的每个像素都和保存前完全相同");

    const std::string jpgFile = (out / "01_part.jpg").string();
    cv::imwrite(jpgFile, part, {cv::IMWRITE_JPEG_QUALITY, 90});
    const cv::Mat fromJpg = cv::imread(jpgFile, cv::IMREAD_UNCHANGED);
    const double maxDiff = cv::norm(part, fromJpg, cv::NORM_INF);  // 相差最大的那个像素差多少
    std::cout << "JPEG（质量 90）：像素值最多相差 " << maxDiff << "；文件大小 PNG "
              << cvtut::fs::file_size(pngFile) << " 字节，JPEG " << cvtut::fs::file_size(jpgFile) << " 字节\n";
    c.check(maxDiff > 0, "JPEG：读回来的像素和保存前不完全相同");

    // ---------- 5. 路径里有中文 ----------
    // u8path：把 UTF-8 编码的字符串转换成路径（第 2 课讲过 /utf-8，源文件里的中文是 UTF-8 编码）
    const cvtut::fs::path chineseDir = out / cvtut::fs::u8path("中文目录");
    cvtut::fs::create_directories(chineseDir);
    const cvtut::fs::path chineseFile = chineseDir / cvtut::fs::u8path("零件.png");

    // 办法：自己读写文件的字节，编码和解码在内存里做
    std::vector<uchar> bytes;
    cv::imencode(".png", part, bytes);                             // 图像 → PNG 格式的字节
    {
        std::ofstream file(chineseFile, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::ifstream file(chineseFile, std::ios::binary);
    const std::vector<uchar> readBack((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const cv::Mat decoded = cv::imdecode(readBack, cv::IMREAD_UNCHANGED);  // PNG 格式的字节 → 图像
    c.check(!decoded.empty() && cv::norm(part, decoded, cv::NORM_INF) == 0, "imencode / imdecode：中文路径读写正常");

    // 对比：直接把 UTF-8 的路径字符串交给 imread。在 Windows 上通常读不到，在 macOS / Linux 上可以。
    const cv::Mat direct = cv::imread(chineseFile.u8string(), cv::IMREAD_UNCHANGED);
    std::cout << "直接用 imread 读中文路径：" << (direct.empty() ? "失败（读到空图像）" : "成功") << "\n";

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
