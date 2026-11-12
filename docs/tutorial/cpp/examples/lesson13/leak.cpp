// 两种内存泄漏。【故意】有错，用来观察泄漏检查工具的报告。
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// ---------- 泄漏一：new 了没有 delete ----------

struct Model {
    std::vector<float> weights = std::vector<float>(1000, 0.0f);  // 4000 字节
};

bool loadAndCheck(bool valid) {
    Model* model = new Model();
    if (!valid) return false;  // ← 提前返回，忘了 delete
    delete model;
    return true;
}

// ---------- 泄漏二：shared_ptr 互相指着对方 ----------

struct Node {
    std::string name;
    std::shared_ptr<Node> next;  // 上游指向下游
    std::shared_ptr<Node> prev;  // 下游又指回上游 ← 形成了环
};

void buildCycle() {
    auto a = std::make_shared<Node>();
    auto b = std::make_shared<Node>();
    a->name = "a";
    b->name = "b";
    a->next = b;
    b->prev = a;
    // 函数结束时，局部变量 a、b 销毁，但 a 被 b->prev 持有，b 被 a->next 持有。
    // 两个计数都还是 1，永远不会归零，两个节点都不会被释放。
}

int main() {
    loadAndCheck(false);
    buildCycle();
    std::cout << "程序正常结束，没有任何报错\n";
    return 0;
}
