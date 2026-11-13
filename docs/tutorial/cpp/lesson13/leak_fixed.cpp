// leak.cpp 的修复版本：unique_ptr 管理独占的对象；用 weak_ptr 打破环。
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct Model {
    std::vector<float> weights = std::vector<float>(1000, 0.0f);
};

bool loadAndCheck(bool valid) {
    const auto model = std::make_unique<Model>();  // 任何路径退出都会自动释放
    if (!valid) return false;
    return true;
}

struct Node {
    std::string name;
    std::shared_ptr<Node> next;  // “拥有”下游
    std::weak_ptr<Node> prev;    // 只是“认识”上游，不拥有：不增加计数
};

bool buildChain() {
    auto a = std::make_shared<Node>();
    auto b = std::make_shared<Node>();
    a->name = "a";
    b->name = "b";
    a->next = b;
    b->prev = a;

    // 使用 weak_ptr 之前要先 lock()：对象还在就得到一个 shared_ptr，已经没了就得到空
    if (const auto upstream = b->prev.lock()) return upstream->name == "a";
    return false;
}

int main() {
    const bool ok = !loadAndCheck(false) && loadAndCheck(true) && buildChain();
    std::cout << (ok ? "全部符合预期，没有泄漏\n" : "有不符合预期的结果\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
