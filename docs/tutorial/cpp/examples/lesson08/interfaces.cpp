// 接口与多态：工位只依赖“图像源”这个接口，不关心图像来自相机还是文件夹。
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct Frame {
    int width = 0;
    int height = 0;
    std::string from;
};

// ---------- 接口：只说“能做什么”，不说“怎么做” ----------

class ImageSource {
public:
    virtual ~ImageSource() = default;              // 接口必须有虚析构函数
    virtual std::optional<Frame> next() = 0;       // = 0：纯虚函数，派生类必须实现
    virtual std::string describe() const = 0;
};

// ---------- 实现一：从文件夹读图 ----------

class FolderSource final : public ImageSource {
public:
    explicit FolderSource(std::vector<std::string> files) : files_(std::move(files)) {}

    std::optional<Frame> next() override {         // override：让编译器确认确实在重写基类的函数
        if (index_ >= files_.size()) return std::nullopt;
        return Frame{640, 480, files_[index_++]};
    }
    std::string describe() const override { return "文件夹（" + std::to_string(files_.size()) + " 张）"; }

private:
    std::vector<std::string> files_;
    std::size_t index_ = 0;
};

// ---------- 实现二：相机 ----------

class CameraSource final : public ImageSource {
public:
    explicit CameraSource(int maxFrames) : remaining_(maxFrames) {}

    std::optional<Frame> next() override {
        if (remaining_ <= 0) return std::nullopt;
        --remaining_;
        return Frame{2592, 1944, "相机"};
    }
    std::string describe() const override { return "相机"; }

private:
    int remaining_;
};

// ---------- 使用者：只认识接口 ----------

class Station {
public:
    // 依赖通过构造函数传进来（依赖注入）。Station 不知道、也不关心具体是哪种图像源。
    explicit Station(std::unique_ptr<ImageSource> source) : source_(std::move(source)) {}

    int runAll() {
        int count = 0;
        while (const auto frame = source_->next()) {  // 运行时才决定调用哪个类的 next()
            ++count;
            pixels_ += static_cast<long long>(frame->width) * frame->height;
        }
        return count;
    }
    long long pixels() const { return pixels_; }
    std::string sourceName() const { return source_->describe(); }

private:
    std::unique_ptr<ImageSource> source_;
    long long pixels_ = 0;
};

// ---------- 测试用的假实现：不需要相机，也不需要文件 ----------

class FakeSource final : public ImageSource {
public:
    explicit FakeSource(int frames) : remaining_(frames) {}
    std::optional<Frame> next() override {
        ++calls;
        if (remaining_ <= 0) return std::nullopt;
        --remaining_;
        return Frame{10, 10, "fake"};
    }
    std::string describe() const override { return "fake"; }

    inline static int calls = 0;  // 记录被调用了多少次

private:
    int remaining_;
};

// ---------- 对象切片：按值传递基类会“切掉”派生类的部分 ----------

struct Shape {
    virtual ~Shape() = default;
    virtual std::string name() const { return "形状"; }
};
struct Circle : Shape {
    std::string name() const override { return "圆"; }
};

std::string nameByValue(Shape s) { return s.name(); }             // 按值：复制时只复制了 Shape 那部分
std::string nameByRef(const Shape& s) { return s.name(); }        // 按引用：保持原来的类型

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

int main() {
    Station fromFolder(std::make_unique<FolderSource>(std::vector<std::string>{"a.png", "b.png", "c.png"}));
    Station fromCamera(std::make_unique<CameraSource>(2));

    std::cout << fromFolder.sourceName() << "：处理了 " << fromFolder.runAll() << " 张\n";
    std::cout << fromCamera.sourceName() << "：处理了 " << fromCamera.runAll() << " 张\n";
    check(fromFolder.pixels() == 3LL * 640 * 480, "文件夹图像源");
    check(fromCamera.pixels() == 2LL * 2592 * 1944, "相机图像源");

    // 用假实现测试 Station 的逻辑
    Station underTest(std::make_unique<FakeSource>(5));
    check(underTest.runAll() == 5, "处理了 5 帧");
    check(FakeSource::calls == 6, "next() 被调用 6 次：5 次有图，第 6 次返回空后停止");

    // 对象切片
    const Circle circle;
    check(nameByRef(circle) == "圆", "按引用传递：调用的是 Circle::name");
    check(nameByValue(circle) == "形状", "按值传递：派生类部分被切掉，调用的是 Shape::name");

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
