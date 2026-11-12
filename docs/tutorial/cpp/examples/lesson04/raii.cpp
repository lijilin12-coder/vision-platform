// RAII 与智能指针：资源在构造时获取，在析构时自动释放。
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// ---------- 一个假想的相机：用计数器模拟“打开/关闭”这种必须成对出现的操作 ----------

int openCameras = 0;  // 当前处于打开状态的相机数量（示例里用全局变量只是为了观察）

int openCamera(const std::string& name) {
    ++openCameras;
    std::cout << "  打开相机 " << name << "\n";
    return 42;  // 句柄
}

void closeCamera(int /*handle*/) {
    --openCameras;
    std::cout << "  关闭相机\n";
}

// ---------- 1. 手动管理：只要中间提前返回或抛异常，就会漏掉关闭 ----------

bool grabManual(bool failInTheMiddle) {
    const int handle = openCamera("手动管理");
    if (failInTheMiddle) return false;  // ← 忘了 closeCamera：相机一直开着
    closeCamera(handle);
    return true;
}

// ---------- 2. RAII：把“关闭”放进析构函数 ----------

class Camera {
public:
    explicit Camera(const std::string& name) : handle_(openCamera(name)) {}
    ~Camera() { closeCamera(handle_); }

    // 相机不能被复制（两个对象不能同时拥有同一个句柄），所以明确禁止拷贝
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    int grab() const { return handle_; }

private:
    int handle_;
};

bool grabRaii(bool failInTheMiddle) {
    Camera camera("RAII");
    if (failInTheMiddle) return false;  // camera 在这里自动析构 → 相机被关闭
    (void)camera.grab();
    return true;
}

void grabAndThrow() {
    Camera camera("抛异常");
    throw std::runtime_error("采集失败");  // 抛异常时，camera 同样会被析构
}

// ---------- 3. RAII 不只管资源：作用域计时器 ----------

class ScopedTimer {
public:
    explicit ScopedTimer(std::string label) : label_(std::move(label)), start_(Clock::now()) {}
    ~ScopedTimer() {
        const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        std::cout << "  [" << label_ << "] 耗时 " << ms << " ms\n";
    }

private:
    using Clock = std::chrono::steady_clock;
    std::string label_;
    Clock::time_point start_;
};

// ---------- 4. unique_ptr：独占所有权 ----------

struct Model {
    explicit Model(std::string n) : name(std::move(n)) { std::cout << "  加载模型 " << name << "\n"; }
    ~Model() { std::cout << "  释放模型 " << name << "\n"; }
    std::string name;
};

// 工厂函数：返回 unique_ptr，明确表示“所有权交给调用者”
std::unique_ptr<Model> loadModel(const std::string& name) { return std::make_unique<Model>(name); }

// 按值接收 unique_ptr：明确表示“我要接管它”
void install(std::unique_ptr<Model> model, std::vector<std::unique_ptr<Model>>& installed) {
    installed.push_back(std::move(model));
}

// ---------- 5. shared_ptr：共享所有权 ----------

struct Pixels {
    explicit Pixels(std::size_t n) : data(n, 0) {}
    std::vector<unsigned char> data;
};

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

int main() {
    std::cout << "1. 手动管理，中途返回：\n";
    grabManual(true);
    check(openCameras == 1, "手动管理漏掉了关闭，应该还有 1 个相机开着");
    std::cout << "  → 还有 " << openCameras << " 个相机没有关闭\n";
    openCameras = 0;

    std::cout << "2. RAII，中途返回：\n";
    grabRaii(true);
    check(openCameras == 0, "RAII 应该自动关闭");

    std::cout << "   RAII，抛出异常：\n";
    try {
        grabAndThrow();
    } catch (const std::exception& e) {
        std::cout << "  捕获异常：" << e.what() << "\n";
    }
    check(openCameras == 0, "抛异常时也应该自动关闭");

    std::cout << "3. 作用域计时器：\n";
    {
        ScopedTimer timer("求和");
        long long sum = 0;
        for (int i = 0; i < 1000000; ++i) sum += i;
        check(sum == 499999500000LL, "求和结果");
    }  // timer 在这里析构，打印耗时

    std::cout << "4. unique_ptr：\n";
    {
        std::vector<std::unique_ptr<Model>> installed;
        std::unique_ptr<Model> model = loadModel("scratch-detector");
        // auto copy = model;                 // 编译错误：unique_ptr 不能复制
        install(std::move(model), installed);  // 所有权转移给 installed
        check(model == nullptr, "转移之后，原来的指针变为空");
        check(installed.size() == 1, "模型已安装");
        std::cout << "  离开作用域……\n";
    }  // installed 析构 → 里面的模型自动释放

    std::cout << "5. shared_ptr：\n";
    {
        std::shared_ptr<const Pixels> a = std::make_shared<Pixels>(5000000);
        check(a.use_count() == 1, "一个持有者");
        {
            std::shared_ptr<const Pixels> b = a;  // 只复制指针，不复制 500 万个字节
            check(a.use_count() == 2, "两个持有者");
            check(a->data.data() == b->data.data(), "指向同一块像素");
        }  // b 析构：计数减一，像素还在
        check(a.use_count() == 1, "又回到一个持有者");
    }  // a 析构：计数归零，像素释放

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
