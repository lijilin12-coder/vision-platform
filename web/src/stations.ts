import { reactive } from "vue";
import type { PlatformClient } from "./platform/PlatformClient";
import { acceptanceLabel, releaseTitle } from "./releases";
import type { InspectionRecord, QualitySummary, Release, SavedRecord, Station } from "./platform/types";

// 工位页面的状态和操作。和编辑器一样：组件只做显示，和后端打交道的事在这里。
//
// 工位在后端持续运行；界面隔一小会儿问一次“有没有新的检测”（poll），只取上次之后新出现的记录。

const kKeptRecords = 60; // 页面上最多留这么多条检测记录
const kHistoryPage = 30; // 历史记录一次取这么多条

// 历史记录：保存在后端磁盘上的检测记录。和上面“刚刚检测的”不同，它在程序重启之后还在，可以往前翻
export interface HistoryState {
  records: SavedRecord[]; // 从新到旧
  summary: QualitySummary | null;
  onlyNg: boolean; // 只看不合格的
  review: "" | "unreviewed" | "misjudged"; // 按复判筛选：都要 / 还没复判的 / 复判确认判错了的
  notice: string | null; // 做成了一件事的提示（例如导出了多少幅图像）
  hasMore: boolean; // 还有更早的没取
  selected: SavedRecord | null; // 点开看的那一条
}

export interface StationsState {
  stations: Station[];
  releases: Release[];
  selectedId: string | null;
  current: Station | null; // 选中的那个工位的最新状态
  records: InspectionRecord[]; // 选中工位的检测记录，从新到旧
  error: string | null;
  history: HistoryState;
}

export function createStations(client: PlatformClient) {
  const state = reactive<StationsState>({ stations: [], releases: [], selectedId: null, current: null, records: [], error: null,
    history: { records: [], summary: null, onlyNg: false, review: "", notice: null, hasMore: false, selected: null } });
  let lastSequence = 0;

  // 取一页历史记录。more：接在已有的后面（翻页）；否则从最新的重新开始
  async function fetchHistory(more: boolean) {
    const id = state.selectedId;
    if (!id) return;
    const h = state.history;
    const before = more && h.records.length > 0 ? h.records[h.records.length - 1].id : undefined;
    const [page, summary] = await Promise.all([
      client.listRecords(id, { verdict: h.onlyNg ? "NG" : undefined, review: h.review || undefined, before, limit: kHistoryPage }),
      client.getSummary(id),
    ]);
    if (id !== state.selectedId) return; // 等结果的时候换了工位：这一页不要了
    h.records = more ? [...h.records, ...page] : page;
    h.hasMore = page.length === kHistoryPage;
    h.summary = summary;
    if (!more) h.selected = null;
  }

  async function attempt(action: () => Promise<void>): Promise<boolean> {
    state.error = null;
    try {
      await action();
      return true;
    } catch (e) {
      state.error = e instanceof Error ? e.message : String(e);
      return false;
    }
  }

  // 把后端给的工位状态收下来；recent 里是新的检测记录
  function absorb(station: Station) {
    if (station.id !== state.selectedId) return;
    for (const r of station.recent) {
      if (r.sequence <= lastSequence) continue;
      lastSequence = r.sequence;
      state.records.unshift(r);
    }
    if (state.records.length > kKeptRecords) state.records.length = kKeptRecords;
    state.current = { ...station, recent: [] };
    const i = state.stations.findIndex((s) => s.id === station.id);
    if (i >= 0) state.stations[i] = state.current;
  }

  // 对选中的工位做一个操作
  function onCurrent(action: (stationId: string) => Promise<Station>) {
    const id = state.selectedId;
    if (!id) return Promise.resolve(false);
    return attempt(async () => absorb(await action(id)));
  }

  return {
    state,

    async refresh() {
      await attempt(async () => {
        [state.stations, state.releases] = await Promise.all([client.listStations(), client.listReleases()]);
      });
    },

    async select(stationId: string) {
      state.selectedId = stationId;
      state.current = null;
      state.records = [];
      state.history = { records: [], summary: null, onlyNg: state.history.onlyNg, review: state.history.review, notice: null, hasMore: false, selected: null };
      lastSequence = 0;
      await attempt(async () => absorb(await client.getStation(stationId, 0)));
    },

    async create(name: string) {
      let created: Station | null = null;
      const ok = await attempt(async () => {
        created = await client.createStation(name);
        state.stations = await client.listStations();
      });
      if (ok && created) await this.select((created as Station).id);
    },

    // 问一次后端：有没有新的检测。页面开着的时候隔一会儿调一次
    async poll() {
      const id = state.selectedId;
      if (!id) return;
      try {
        absorb(await client.getStation(id, lastSequence));
      } catch {
        // 轮询偶尔失败不打扰人；真的连不上了，下一次用户操作时会看到
      }
    },

    bind: (directory: string) => onCurrent((id) => client.bindImageFolder(id, directory)),
    deploy: (releaseId: string) => onCurrent((id) => client.deploy(id, releaseId)),
    rollback: () => onCurrent((id) => client.rollback(id)),
    start: () => onCurrent((id) => client.startStation(id)),
    stop: () => onCurrent((id) => client.stopStation(id)),

    // 上线包给人看的名字：方案名 + 方案的版本 + 上线包编号的开头。
    // 同一个方案发布过几次、各做了上线包时，靠版本号分得清谁是谁
    releaseName(releaseId: string): string {
      if (!releaseId) return "（无）";
      const r = state.releases.find((x) => x.id === releaseId);
      if (!r) return releaseId.slice(0, 8);
      // 没验收的、没通过的标出来：没通过的后端不让部署，在下拉框里就该看得出
      return releaseTitle(r) + (r.acceptanceState === "passed" ? "" : " · " + acceptanceLabel(r));
    },

    // ---- 历史记录 ----
    loadHistory: () => attempt(() => fetchHistory(false)),
    loadMoreHistory: () => attempt(() => fetchHistory(true)),
    setOnlyNg(onlyNg: boolean) {
      state.history.onlyNg = onlyNg;
      return attempt(() => fetchHistory(false));
    },
    setReviewFilter(review: "" | "unreviewed" | "misjudged") {
      state.history.review = review;
      return attempt(() => fetchHistory(false));
    },

    // ---- 复判 ----
    // 复判点开的那条记录：人确认它实际是合格还是不合格。可以改，以最近的一次为准
    async review(truth: "OK" | "NG", note = "") {
      const id = state.selectedId;
      const picked = state.history.selected;
      if (!id || !picked) return false;
      return attempt(async () => {
        const updated = await client.reviewRecord(id, picked.id, truth, note.trim());
        // 列表里的那一条换成带着复判的；不重新取整页——人正在一条条往下复判，列表不能跳
        const i = state.history.records.findIndex((r) => r.id === updated.id);
        if (i >= 0) state.history.records[i] = updated;
        if (state.history.selected?.id === updated.id) state.history.selected = updated;
        state.history.summary = await client.getSummary(id);
      });
    },
    // 复判完一条，点开下一条（更早的那条）。没有下一条时留在原地
    pickNext() {
      const h = state.history;
      const i = h.records.findIndex((r) => r.id === h.selected?.id);
      if (i >= 0 && i + 1 < h.records.length) h.selected = h.records[i + 1];
    },
    // 把复判过的图像导出成两个目录（ok / ng），可以直接拿去建基准图集
    async exportReviewed(directory: string) {
      const id = state.selectedId;
      if (!id || !directory.trim()) return false;
      return attempt(async () => {
        const r = await client.exportReviewed(id, directory.trim());
        state.history.notice = `已导出到 ${directory.trim()}：ok ${r.ok} 幅，ng ${r.ng} 幅` +
          (r.withoutImage > 0 ? `；另有 ${r.withoutImage} 条复判过的记录没有图像（当时没留，或者已经过期）` : "");
      });
    },

    pickRecord(record: SavedRecord | null) {
      state.history.selected = record;
    },
    // 当时的图像；没有（当时没留，或者过期清掉了）时返回空
    recordImageUrl: (record: SavedRecord) =>
      state.selectedId && (record.imageState === "kept" || record.imageState === "cached") ? client.recordImageUrl(state.selectedId, record.id) : "",
    // 良率给人看的样子
    yieldText(summary: QualitySummary | null): string {
      return summary?.yield == null ? "—" : (summary.yield * 100).toFixed(1) + "%";
    },

    frameUrl: (stationId: string, sequence: number) => client.stationFrameUrl(stationId, sequence),
    dismiss() {
      state.error = null;
      state.history.notice = null;
    },
  };
}

export type Stations = ReturnType<typeof createStations>;

// 复判的结论，给人看
export function reviewLabel(review: { kind: string } | null): string {
  if (!review) return "";
  return review.kind === "false_alarm" ? "误报" : review.kind === "miss" ? "漏检" : review.kind === "was_error" ? "已补判" : "判对了";
}

// 一条记录的图像为什么看不到 / 还能看多久，给人看
export function imageNote(record: { imageState: string; review: unknown }): string {
  if (record.imageState === "cached") return "这幅图像在短期缓存里，过了保留期会自动清掉。复判之后会长期保留。";
  if (record.imageState === "expired") return "这幅图像已经过了保留期，被清掉了。检测记录还在。";
  if (record.imageState === "none") return "这一次没有保留图像。";
  return "";
}
