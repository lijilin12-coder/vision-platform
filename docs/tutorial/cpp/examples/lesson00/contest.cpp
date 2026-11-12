// 竞赛写法：数一数网格里有几个连通块（'1' 表示前景，4 邻域）。
// 这种写法在比赛里完全没问题：只跑一次、只有自己看、越快写完越好。
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
