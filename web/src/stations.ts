import { reactive } from "vue";
import type { PlatformClient } from "./platform/PlatformClient";
import type { InspectionRecord, Release, Station } from "./platform/types";

// 工位页面的状态和操作。和编辑器一样：组件只做显示，和后端打交道的事在这里。
//
// 工位在后端持续运行；界面隔一小会儿问一次“有没有新的检测”（poll），只取上次之后新出现的记录。

const kKeptRecords = 60; // 页面上最多留这么多条检测记录

export interface StationsState {
  stations: Station[];
  releases: Release[];
  selectedId: string | null;
  current: Station | null; // 选中的那个工位的最新状态
  records: InspectionRecord[]; // 选中工位的检测记录，从新到旧
  error: string | null;
}

export function createStations(client: PlatformClient) {
  const state = reactive<StationsState>({ stations: [], releases: [], selectedId: null, current: null, records: [], error: null });
  let lastSequence = 0;

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
      const version = r.recipeVersionId.includes("@") ? " " + r.recipeVersionId.slice(r.recipeVersionId.lastIndexOf("@") + 1) : "";
      return `${r.recipeName}${version}（${releaseId.slice(0, 8)}）`;
    },

    frameUrl: (stationId: string, sequence: number) => client.stationFrameUrl(stationId, sequence),
    dismiss() {
      state.error = null;
    },
  };
}

export type Stations = ReturnType<typeof createStations>;
