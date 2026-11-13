# 第 0 课：从竞赛 C++ 到工程 C++

> 预计用时：1.5 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 1. 同一道题，两种写法

题目：给一个由 `0` 和 `1` 组成的网格，数一数有几个连通块（上下左右相连算一块）。

这道题你大概已经很熟了：BFS 模板题。它也正是本项目里“Blob 分析”算子做的事情，区别只是把网格换成了图像。

### 竞赛写法

[`contest.cpp`](contest.cpp)：

```cpp
#include <bits/stdc++.h>
using namespace std;

const int N = 1005;
int n, m, ans;
char g[N][N];
bool vis[N][N];
int dx[] = {0, 0, 1, -1}, dy[] = {1, -1, 0, 0};

void bfs(int sx, int sy) {
    queue<pair<int, int>> q;
    q.push({sx, sy});
    vis[sx][sy] = 1;
    while (q.size()) {
        auto t = q.front();
        q.pop();
        for (int i = 0; i < 4; i++) {
            int a = t.first + dx[i], b = t.second + dy[i];
            if (a < 0 || a >= n || b < 0 || b >= m) continue;
            if (g[a][b] != '1' || vis[a][b]) continue;
            vis[a][b] = 1;
            q.push({a, b});
        }
    }
}

int main() {
    cin >> n >> m;
    for (int i = 0; i < n; i++) cin >> g[i];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++)
            if (g[i][j] == '1' && !vis[i][j]) bfs(i, j), ans++;
    cout << ans << endl;
}
```

**在比赛里，这段代码没有任何问题。** 它短、写得快、能通过。竞赛代码的目标就是：在限定时间内，对一组确定的输入，算出正确答案，然后再也不看它。

### 把它放进我们的项目会怎样

现在设想把这段代码原样用到视觉平台里：

| 场景 | 会发生什么 |
|---|---|
| 相机每秒送来 20 张图，每张都要数一次 | 第二次调用时 `vis` 和 `ans` 还留着上一次的值，**结果是错的** |
| 两个工位同时运行 | 两个线程同时读写同一个 `g` 和 `vis`，**结果随机出错，偶尔崩溃** |
| 图像是 2592×1944 | 超过了 `N = 1005`，**数组越界**。程序可能崩溃，也可能悄悄算出错误的结果 |
| 输入的某一行少了几个字符 | 没有任何检查，带着错误的数据继续算 |
| 想写个测试验证它对不对 | 它从 `cin` 读、往 `cout` 写，**没法在程序里直接调用它并检查结果** |
| 三个月后同事来改 | `g`、`a`、`b`、`t` 是什么？要读完整段代码才知道 |
| 在 Mac 或 Windows 上编译 | **编译不过**：`<bits/stdc++.h>` 是 GCC 特有的，Mac 默认的编译器和 Windows 上微软的编译器都没有这个文件 |

这些问题没有一个是算法错误。**算法是对的，但代码承受不了“被反复使用、被别人修改、在不同环境里运行”。** 工程 C++ 要解决的就是这些问题。

### 工程写法

同样的算法，拆成了四个文件：

| 文件 | 作用 |
|---|---|
| [`blob_count.h`](blob_count.h) | 头文件：声明这个模块**能做什么** |
| [`blob_count.cpp`](blob_count.cpp) | 源文件：**怎么做** |
| [`main.cpp`](main.cpp) | 程序入口：只负责读输入、调用、输出 |
| [`test.cpp`](test.cpp) | 测试：用几组数据检查结果 |

先看头文件，它是这个模块给别人看的“说明书”：

```cpp
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lesson00 {

class BinaryGrid {
public:
    static std::optional<BinaryGrid> fromLines(const std::vector<std::string>& lines);

    std::size_t rows() const { return rows_; }
    std::size_t cols() const { return cols_; }
    bool at(std::size_t row, std::size_t col) const { return cells_[row * cols_ + col]; }

private:
    BinaryGrid(std::size_t rows, std::size_t cols, std::vector<bool> cells)
        : rows_(rows), cols_(cols), cells_(std::move(cells)) {}

    std::size_t rows_;
    std::size_t cols_;
    std::vector<bool> cells_;
};

int countBlobs(const BinaryGrid& grid);

}  // namespace lesson00
```

这里有不少你可能没见过的写法（`optional`、`const&`、`std::move`、`static` 成员函数）。**现在不需要全部看懂**，后面每一课会讲其中一两个。这一课只需要看出它和竞赛写法在思路上的 7 个区别。

## 2. 七个区别

### 区别 1：不用全局变量，数据从参数进、从返回值出

```cpp
// 竞赛：输入、输出、中间状态都是全局变量
int n, m, ans;  char g[N][N];  bool vis[N][N];
void bfs(int sx, int sy);

// 工程：输入是参数，输出是返回值，中间状态是函数内的局部变量
int countBlobs(const BinaryGrid& grid);
```

`countBlobs` 只依赖传给它的 `grid`。所以它可以被调用任意多次，可以被多个线程同时调用，可以在测试里直接调用。看函数的声明就知道它需要什么、给出什么。

### 区别 2：大小由数据决定，不猜一个上限

```cpp
char g[1005][1005];           // 竞赛：题目保证 n ≤ 1000
std::vector<bool> cells_;     // 工程：有多少存多少
```

工程里没有“题目保证”。用 `std::vector`，大小在运行时根据实际输入确定。

### 区别 3：把数据和它的规则放在一起

网格有一条规则：**每一行必须一样长**。竞赛写法里没有地方表达这条规则。工程写法里，`BinaryGrid` 是一个类：

- 构造函数是 `private` 的，外面的代码**只能通过 `fromLines` 创建**它；
- `fromLines` 会检查每一行的长度，不合格就创建失败；
- 所以，**只要你手里有一个 `BinaryGrid`，它就一定是合法的**。`countBlobs` 里不需要再做任何检查。

项目里的 `Roi`（宽高必须大于 0）、`Recipe`（不能有环）用的是同一个思路。第 1 课和第 5 课会详细讲。

### 区别 4：出错要明确地告诉调用者

```cpp
static std::optional<BinaryGrid> fromLines(...);   // 可能创建失败
```

`std::optional<BinaryGrid>` 的意思是“可能有一个 `BinaryGrid`，也可能没有”。调用者必须先检查有没有，才能使用。这比“带着错误的数据继续算”要好得多：错误在它发生的地方就被发现。第 6 课会讲 `optional` 和项目里的 `Result`。

### 区别 5：名字要让人看得懂

| 竞赛 | 工程 |
|---|---|
| `g` | `grid` |
| `vis` | `visited` |
| `t` | `current` |
| `a`, `b` | `row`, `col` |
| `bfs` | `markBlob`（标记一个连通块） |

名字长一点没有关系。代码写一次，会被读很多次。

### 区别 6：不写 `using namespace std;`，把自己的代码放进命名空间

`using namespace std;` 会把标准库里几千个名字全部引进来。竞赛里没关系，工程里会出问题。

本项目就真实地遇到过一次：测试代码里写了一个辅助函数叫 `ref`，编译时却报了一大片错误，说在用“已删除的函数 `std::ref`”。原因是标准库里恰好也有一个 `std::ref`，编译器选错了。最后是把函数改名为 `port` 才解决的。这还是在**没有**写 `using namespace std;` 的情况下发生的，写了的话这类冲突会多得多。

所以工程代码里：

- 标准库的东西写全称：`std::vector`、`std::string`。
- 自己的代码放进自己的命名空间：示例里是 `namespace lesson00`，项目里是 `namespace vision`。

### 区别 7：只包含用到的头文件

```cpp
#include <bits/stdc++.h>     // 竞赛：一次包含整个标准库
#include <queue>             // 工程：用什么包含什么
#include <vector>
```

`<bits/stdc++.h>` 有两个问题：它不是标准的一部分，换个编译器就可能没有（Mac 和 Windows 上都没有）；它会让每个文件的编译时间变长，项目有几百个文件时差别非常明显。

## 3. 换来了什么：可以测试

因为 `countBlobs` 不依赖全局变量、不读 `cin`，所以可以这样检查它（[`test.cpp`](test.cpp)）：

```cpp
assert(countBlobs(*BinaryGrid::fromLines({"000", "000"})) == 0);
assert(countBlobs(*BinaryGrid::fromLines({"111", "111"})) == 1);
assert(countBlobs(*BinaryGrid::fromLines({"101", "010", "101"})) == 5);   // 斜着不算相连

const auto grid = *BinaryGrid::fromLines({"10", "01"});
assert(countBlobs(grid) == 2);
assert(countBlobs(grid) == 2);        // 连续调用两次，结果互不影响

assert(!BinaryGrid::fromLines({"11", "1"}).has_value());   // 各行长度不一致：创建失败
```

以后每次修改代码，重新跑一遍这些测试，几毫秒就知道有没有改坏。本项目目前有 50 多个这样的测试。

## 4. 动手运行

```bash
cd docs/tutorial/cpp/lesson00

# 工程写法
clang++ -std=c++17 -Wall -Wextra main.cpp blob_count.cpp -o blob_count
./blob_count < input.txt          # 输出 2

# 测试
clang++ -std=c++17 -Wall -Wextra test.cpp blob_count.cpp -o blob_test
./blob_test                        # 输出“全部通过”
```

Linux 上把 `clang++` 换成 `g++`。`-Wall -Wextra` 是打开编译器警告，工程代码要求**没有任何警告**。

Windows 上，在“x64 Native Tools Command Prompt for VS 2022”里：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 main.cpp blob_count.cpp /Fe:blob_count.exe
blob_count.exe < input.txt

cl /std:c++17 /W4 /EHsc /utf-8 test.cpp blob_count.cpp /Fe:blob_test.exe
blob_test.exe
```

竞赛写法 `contest.cpp` 在 Linux 上可以用 `g++ contest.cpp` 编译；在 Mac 和 Windows 上会因为找不到 `<bits/stdc++.h>` 而失败，这正好印证了区别 7。

## 5. 代价是什么

工程写法并不是“更好的写法”，它是**为另一个目标服务的写法**：

| | 竞赛写法 | 工程写法 |
|---|---|---|
| 代码量 | 约 35 行，1 个文件 | 约 160 行，4 个文件 |
| 写完要多久 | 5 分钟 | 30 分钟 |
| 适合 | 只跑一次、只有自己看 | 被反复调用、被别人修改、长期运行 |

**准备蓝桥杯时，继续用竞赛写法。** 那是那个场景下正确的选择。两种写法都要会，知道什么时候用哪种。

## 6. 练习

1. **运行一遍**：按第 4 节编译并运行工程写法和测试。
2. **读代码**：打开 [`blob_count.cpp`](blob_count.cpp)，找到和竞赛写法里 `dx[]`、`dy[]` 对应的部分。它叫什么名字？放在了哪里？为什么不再是全局变量？
3. **改代码**：给 `countBlobs` 增加第二个参数 `bool eightConnected`，为 `true` 时斜着相邻也算相连。在 `test.cpp` 里补两条测试：`{"101", "010", "101"}` 在 4 邻域下是 5 块，在 8 邻域下是 1 块。
4. **看项目里的代码**：打开 [`operators/src/blob.cpp`](../../../../operators/src/blob.cpp)，这是项目里真正的 Blob 分析算子。不用看懂全部，回答：它的输入是什么类型？输出有哪两个？连通块是它自己算的，还是调用了别的库？

## 7. 自测题

1. 竞赛写法里的 `bfs` 函数，如果在一个程序里对两张不同的网格各调用一遍，为什么第二次的结果可能是错的？
2. `BinaryGrid` 的构造函数为什么是 `private` 的？
3. `fromLines` 的返回类型是 `std::optional<BinaryGrid>`，而不是 `BinaryGrid`，它想告诉调用者什么？
4. 为什么工程代码里不写 `using namespace std;`？
5. 什么情况下你仍然应该用竞赛写法？

## 下一课

[第 1 课：面向对象基础——自己写一个字符串类](../lesson01/README.md)：为什么要把数据放进 `private`，构造、析构、拷贝、移动各自解决什么问题。
