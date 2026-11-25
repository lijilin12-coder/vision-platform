import { reactive } from "vue";
import type { PlatformClient } from "./platform/PlatformClient";
import type { Acceptance, GoldenSet, NewGoldenSet, Release, WrongCase } from "./platform/types";

// “上线包”页面的状态和操作：上线包的列表、基准图集、验收检查。
//
// 验收检查：把上线包在一个基准图集（带着人确认的真实判定）上跑一遍，按验收标准得出通过 / 没通过。
// 没通过的上线包部署不了——这条规则在后端，这里只负责把结果显示清楚。

export interface ReleasesState {
  releases: Release[];
  goldenSets: GoldenSet[];
  selectedId: string | null; // 选中的上线包
  goldenSetId: string; // 验收时用哪个基准图集
  checking: boolean; // 验收检查正在跑（用例多的时候要等一会儿）
  error: string | null;
  notice: string | null; // 做成了一件事的提示（例如导出到了哪里）
}

export function createReleases(client: PlatformClient) {
  const state = reactive<ReleasesState>({ releases: [], goldenSets: [], selectedId: null, goldenSetId: "", checking: false, error: null, notice: null });

  async function attempt(action: () => Promise<void>): Promise<boolean> {
    state.error = null;
    state.notice = null;
    try {
      await action();
      return true;
    } catch (e) {
      state.error = e instanceof Error ? e.message : String(e);
      return false;
    }
  }

  async function load() {
    [state.releases, state.goldenSets] = await Promise.all([client.listReleases(), client.listGoldenSets()]);
    if (!state.releases.some((r) => r.id === state.selectedId)) state.selectedId = state.releases[0]?.id ?? null;
    if (!state.goldenSets.some((g) => g.id === state.goldenSetId)) state.goldenSetId = state.goldenSets[0]?.id ?? "";
  }

  return {
    state,
    refresh: () => attempt(load),
    select(releaseId: string) {
      state.selectedId = releaseId;
    },
    selected(): Release | null {
      return state.releases.find((r) => r.id === state.selectedId) ?? null;
    },

    async createGoldenSet(definition: NewGoldenSet) {
      return attempt(async () => {
        const created = await client.createGoldenSet(definition);
        await load();
        state.goldenSetId = created.id; // 刚建的那个，接下来多半就是要用它
      });
    },

    // 给选中的上线包做验收检查
    async check() {
      const releaseId = state.selectedId;
      if (!releaseId || !state.goldenSetId || state.checking) return false;
      state.checking = true;
      const ok = await attempt(async () => {
        await client.checkAcceptance(releaseId, state.goldenSetId);
        await load(); // 结果记在上线包上：重新取一遍，列表里的状态也跟着变
      });
      state.checking = false;
      return ok;
    },

    // 把选中的上线包导出成上线包文件（带到产线上导入）。文件写在后端那台机器上
    async exportTo(path: string) {
      const releaseId = state.selectedId;
      if (!releaseId || !path.trim()) return false;
      return attempt(async () => {
        await client.exportRelease(releaseId, path.trim());
        state.notice = `已导出到 ${path.trim()}。带到产线上：vision-line release import <这个文件>`;
      });
    },

    dismiss() {
      state.error = null;
      state.notice = null;
    },
  };
}

export type Releases = ReturnType<typeof createReleases>;

// ---- 给人看的文字。纯函数，页面和工位页面都用 ----

export function releaseTitle(r: Release): string {
  const version = r.recipeVersionId.includes("@") ? " " + r.recipeVersionId.slice(r.recipeVersionId.lastIndexOf("@") + 1) : "";
  return `${r.recipeName}${version}（${r.id.slice(0, 8)}）`;
}

export function acceptanceLabel(r: Release): string {
  if (r.withdrawn) return "已撤回";
  return r.acceptanceState === "passed" ? "验收通过" : r.acceptanceState === "failed" ? "验收没有通过" : "未验收";
}

export const percent = (rate: number) => (rate * 100).toFixed(1) + "%";

// 一句话说清结论：为什么通过 / 哪一条没达到
export function verdictLine(a: Acceptance): string {
  if (a.passed) return `通过：${a.total} 个用例，漏检 ${a.misses}、误报 ${a.falseAlarms}、出错 ${a.errors}，都在验收标准之内`;
  const over: string[] = [];
  if (a.misses > a.criteria.maxMisses) over.push(`漏检 ${a.misses} 张（最多允许 ${a.criteria.maxMisses}）`);
  if (a.falseAlarmRate > a.criteria.maxFalseAlarmRate + 1e-12) over.push(`误报率 ${percent(a.falseAlarmRate)}（最多允许 ${percent(a.criteria.maxFalseAlarmRate)}）`);
  if (a.errors > a.criteria.maxErrors) over.push(`出错 ${a.errors} 张（最多允许 ${a.criteria.maxErrors}）`);
  return "没有通过：" + over.join("；");
}

export function wrongKind(w: WrongCase): string {
  return w.kind === "miss" ? "漏检" : w.kind === "false_alarm" ? "误报" : "出错";
}
