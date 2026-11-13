// 数一数：不同的传参和使用方式，各发生了几次拷贝、几次移动。
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

// 一个会“记账”的类型：每次被拷贝或移动，就把计数加一。
struct Tracked {
    static inline int copies = 0;
    static inline int moves = 0;
    static void reset() { copies = moves = 0; }

    std::string name;

    explicit Tracked(std::string n) : name(std::move(n)) {}
    Tracked(const Tracked& other) : name(other.name) { ++copies; }
    Tracked(Tracked&& other) noexcept : name(std::move(other.name)) { ++moves; }
    Tracked& operator=(const Tracked& other) {
        name = other.name;
        ++copies;
        return *this;
    }
    Tracked& operator=(Tracked&& other) noexcept {
        name = std::move(other.name);
        ++moves;
        return *this;
    }
};

std::size_t lengthByValue(Tracked t) { return t.name.size(); }            // 传值：调用者的对象被复制一份
std::size_t lengthByConstRef(const Tracked& t) { return t.name.size(); }  // 传 const 引用：不复制，也不能修改
void renameByRef(Tracked& t) { t.name += "!"; }                           // 传引用：不复制，可以修改调用者的对象

// 这个函数要“留下”参数（存进成员或容器）。按值接收，再 move 进去：
// 调用者传左值时拷贝 1 次，传 std::move(x) 或临时对象时 0 次拷贝。
struct Holder {
    std::vector<Tracked> items;
    void add(Tracked t) { items.push_back(std::move(t)); }
};

int failures = 0;
void expect(const char* what, int copies, int moves, int expectedCopies, int expectedMoves) {
    const bool ok = copies == expectedCopies && moves == expectedMoves;
    std::cout << (ok ? "  " : "✗ ") << what << "：拷贝 " << copies << " 次，移动 " << moves << " 次\n";
    if (!ok) ++failures;
}

int main() {
    Tracked a("blob");

    Tracked::reset();
    (void)lengthByValue(a);
    expect("传值", Tracked::copies, Tracked::moves, 1, 0);

    Tracked::reset();
    (void)lengthByConstRef(a);
    expect("传 const 引用", Tracked::copies, Tracked::moves, 0, 0);

    Tracked::reset();
    renameByRef(a);
    expect("传引用", Tracked::copies, Tracked::moves, 0, 0);

    Tracked::reset();
    (void)lengthByValue(std::move(a));  // 明确表示“a 的内容我不要了”
    expect("传值 + std::move", Tracked::copies, Tracked::moves, 0, 1);
    a = Tracked("blob");  // a 被移走后重新赋值，才能继续使用

    std::vector<Tracked> list;
    list.reserve(8);  // 预留空间，避免扩容时的移动干扰计数
    for (int i = 0; i < 3; ++i) list.emplace_back("item");

    Tracked::reset();
    std::size_t total = 0;
    for (auto item : list) total += item.name.size();  // auto：每个元素都复制一份
    expect("for (auto item : list)，3 个元素", Tracked::copies, Tracked::moves, 3, 0);

    Tracked::reset();
    for (const auto& item : list) total += item.name.size();  // const auto&：不复制
    expect("for (const auto& item : list)", Tracked::copies, Tracked::moves, 0, 0);
    (void)total;

    Holder holder;
    holder.items.reserve(8);

    Tracked::reset();
    holder.add(a);  // 调用者还要继续用 a
    expect("add(a)", Tracked::copies, Tracked::moves, 1, 1);

    Tracked::reset();
    holder.add(std::move(a));  // 调用者不再需要 a
    expect("add(std::move(a))", Tracked::copies, Tracked::moves, 0, 2);

    Tracked::reset();
    holder.add(Tracked("temp"));  // 临时对象
    expect("add(Tracked(\"temp\"))", Tracked::copies, Tracked::moves, 0, 1);

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
