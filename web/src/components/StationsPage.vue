<script setup lang="ts">
import { computed, inject, onMounted, onUnmounted, ref } from "vue";
import type { Stations } from "../stations";

// 工位页面：工位列表、部署和回滚、绑定图像目录、开始 / 停止、最近检测的图像和结果。
const stations = inject<Stations>("stations")!;
const state = stations.state;
const newName = ref("");
const folder = ref("");
const releaseToDeploy = ref("");
// 缺陷的坐标是图像上的像素：叠加层要知道图像有多大。图像加载完时记下来
const viewBox = ref("0 0 640 480");
function onFrameLoad(event: Event) {
  const img = event.target as HTMLImageElement;
  viewBox.value = `0 0 ${img.naturalWidth} ${img.naturalHeight}`;
}
let timer: ReturnType<typeof setInterval> | null = null;

onMounted(async () => {
  await stations.refresh();
  if (!state.selectedId && state.stations.length > 0) await select(state.stations[0].id);
  // 隔半秒问一次后端有没有新的检测。页面离开时停掉
  timer = setInterval(() => void stations.poll(), 500);
});
onUnmounted(() => timer && clearInterval(timer));

async function select(id: string) {
  await stations.select(id);
  folder.value = state.current?.imageSource.location ?? "";
}
async function create() {
  const name = newName.value.trim();
  if (!name) return;
  await stations.create(name);
  newName.value = "";
  folder.value = "";
}

const current = computed(() => state.current);
const latest = computed(() => state.records[0] ?? null);
const usable = computed(() => state.releases.filter((r) => !r.withdrawn));
const stateNames: Record<string, string> = { stopped: "已停止", running: "运行中", paused: "已暂停", faulted: "故障" };
</script>

<template>
  <div class="page">
    <aside class="list">
      <h3>工位</h3>
      <div v-for="s in state.stations" :key="s.id" class="station" :class="{ active: s.id === state.selectedId }" :data-station="s.name" @click="select(s.id)">
        <span>{{ s.name }}</span>
        <span class="dot" :class="{ on: s.runner.running }" :title="s.runner.running ? '正在运行' : '没有在运行'" />
      </div>
      <p v-if="state.stations.length === 0" class="muted">还没有工位。</p>
      <div class="new">
        <input v-model="newName" placeholder="新工位的名称" data-station-name @keyup.enter="create" />
        <button :disabled="!newName.trim()" data-station-new @click="create">新建</button>
      </div>
    </aside>

    <section class="detail" v-if="current">
      <div v-if="state.error" class="message" data-station-error @click="stations.dismiss()">{{ state.error }} <span class="close">点一下关闭</span></div>

      <div class="head">
        <strong class="name">{{ current.name }}</strong>
        <span class="state" :class="current.state" data-station-state>{{ stateNames[current.state] }}</span>
        <span class="muted" v-if="current.runner.running && current.runner.idle">等待新图像…</span>
        <span class="spacer" />
        <button class="primary" v-if="!current.runner.running" data-station-start @click="stations.start()">开始</button>
        <button v-else data-station-stop @click="stations.stop()">停止</button>
      </div>
      <div v-if="current.runner.stoppedReason && !current.runner.running" class="muted">上次自己停下了：{{ current.runner.stoppedReason }}</div>

      <div class="settings">
        <div class="row">
          <span class="label">图像目录</span>
          <input v-model="folder" placeholder="后端那台机器上的一个目录，例如 ./demo-images" data-station-folder :disabled="current.runner.running" />
          <button :disabled="current.runner.running || !folder.trim()" data-station-bind @click="stations.bind(folder.trim())">绑定</button>
          <span class="muted" v-if="current.runner.running">运行中不能换</span>
        </div>
        <div class="row">
          <span class="label">生效的上线包</span>
          <span data-station-active>{{ stations.releaseName(current.activeRelease) }}</span>
          <span v-if="current.pendingRelease" class="pending">
            → 待切换：{{ stations.releaseName(current.pendingRelease) }}{{ current.pendingReady ? "（已就绪，下一次检测前生效）" : "（加载中）" }}
          </span>
        </div>
        <div class="row">
          <span class="label">部署</span>
          <select v-model="releaseToDeploy" data-station-release>
            <option value="" disabled>选一个上线包…</option>
            <option v-for="r in usable" :key="r.id" :value="r.id">{{ stations.releaseName(r.id) }}</option>
          </select>
          <button :disabled="!releaseToDeploy" data-station-deploy @click="stations.deploy(releaseToDeploy)">部署</button>
          <button :disabled="current.releaseHistory.length === 0" data-station-rollback @click="stations.rollback()">回滚到上一个</button>
          <span class="muted" v-if="usable.length === 0">还没有上线包：在“方案编辑”里发布方案，再点“做成上线包”</span>
        </div>
      </div>

      <div class="counts">
        <span class="count ok" data-count-ok>OK {{ current.runner.ok }}</span>
        <span class="count ng" data-count-ng>NG {{ current.runner.ng }}</span>
        <span class="count error">ERROR {{ current.runner.error }}</span>
        <span class="muted" v-if="current.runner.unreadable > 0">读不出来的图像 {{ current.runner.unreadable }} 幅</span>
      </div>

      <div class="live">
        <div class="frame-area">
          <template v-if="latest">
            <div class="caption">
              #{{ latest.sequence }} {{ latest.frameName }}
              <span class="badge" :class="latest.verdict">{{ latest.verdict }}</span>
              <span class="muted">{{ latest.totalMs.toFixed(1) }} ms</span>
            </div>
            <div class="frame">
              <img :src="stations.frameUrl(current.id, latest.sequence)" :alt="latest.frameName" data-station-frame @load="onFrameLoad" />
              <svg v-if="latest.result.defects.length > 0" class="overlay" preserveAspectRatio="none" :viewBox="viewBox">
                <rect v-for="(d, i) in latest.result.defects" :key="i" :x="d.left - 2" :y="d.top - 2" :width="d.width + 4" :height="d.height + 4" class="defect" />
              </svg>
            </div>
            <div v-for="(r, i) in latest.reasons" :key="i" :class="r.pass ? 'pass' : 'fail'">{{ r.pass ? "✓" : "✗" }} {{ r.text }}</div>
          </template>
          <div v-else class="muted empty">还没有检测。绑定图像目录、部署上线包，然后点“开始”。</div>
        </div>
        <div class="records">
          <table>
            <thead><tr><th>#</th><th>图像</th><th>判定</th><th>原因</th><th>上线包</th><th>耗时</th></tr></thead>
            <tbody>
              <template v-for="r in state.records" :key="r.sequence">
                <tr :data-record="r.sequence">
                  <td>{{ r.sequence }}</td>
                  <td>{{ r.frameName }}</td>
                  <td><span class="badge" :class="r.verdict">{{ r.verdict }}</span></td>
                  <td class="reason">{{ r.reasons.filter((x) => !x.pass).map((x) => x.text).join("；") }}</td>
                  <td class="muted">{{ r.releaseId.slice(0, 8) }}</td>
                  <td class="muted">{{ r.totalMs.toFixed(1) }} ms</td>
                </tr>
                <tr v-if="r.swapped" class="swap" data-swapped><td colspan="6">↑ 从这一次开始，换上了上线包 {{ stations.releaseName(r.releaseId) }}</td></tr>
              </template>
            </tbody>
          </table>
        </div>
      </div>
    </section>
    <section class="detail" v-else><p class="muted">在左边新建或选一个工位。</p></section>
  </div>
</template>

<style scoped>
.page { display: grid; grid-template-columns: 200px 1fr; height: 100%; min-height: 0; }
.list { background: var(--panel); border-right: 1px solid var(--border); padding: 10px; overflow-y: auto; }
.station { display: flex; justify-content: space-between; align-items: center; padding: 6px 8px; border-radius: 4px; cursor: pointer; }
.station:hover, .station.active { background: #eff6ff; }
.dot { width: 9px; height: 9px; border-radius: 50%; background: #cbd5e1; }
.dot.on { background: var(--ok); }
.new { display: flex; gap: 4px; margin-top: 10px; }
.new input { flex: 1; }
.detail { padding: 12px 16px; display: flex; flex-direction: column; gap: 10px; min-width: 0; min-height: 0; overflow-y: auto; }
.message { background: #fef2f2; color: #b91c1c; border: 1px solid #fecaca; border-radius: 4px; padding: 6px 10px; cursor: pointer; }
.close { float: right; opacity: 0.6; font-size: 11px; }
.head { display: flex; align-items: center; gap: 10px; }
.name { font-size: 16px; }
.state { padding: 1px 8px; border-radius: 10px; background: #e5e7eb; }
.state.running { background: #dcfce7; color: #15803d; }
.state.faulted { background: #fee2e2; color: #b91c1c; }
.spacer { flex: 1; }
.settings { background: var(--panel); border: 1px solid var(--border); border-radius: 6px; padding: 8px 12px; display: flex; flex-direction: column; gap: 6px; }
.row { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.label { width: 90px; color: var(--muted); }
.row input { flex: 1; max-width: 420px; }
.pending { color: #b45309; }
.counts { display: flex; gap: 14px; align-items: center; }
.count { font-size: 18px; font-weight: 600; }
.count.ok { color: var(--ok); }
.count.ng { color: var(--ng); }
.count.error { color: var(--error); }
.live { display: grid; grid-template-columns: minmax(280px, 42%) 1fr; gap: 14px; min-height: 0; flex: 1; }
.frame-area { min-width: 0; }
.caption { display: flex; align-items: center; gap: 8px; margin-bottom: 4px; font-weight: 600; }
.frame { position: relative; display: inline-block; max-width: 100%; }
.frame img { display: block; max-width: 100%; max-height: 46vh; background: #000; }
.overlay { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
.defect { fill: none; stroke: #ff3b30; stroke-width: 2; vector-effect: non-scaling-stroke; }
.pass { color: var(--ok); }
.fail { color: var(--ng); }
.empty { padding: 30px 0; }
.records { overflow-y: auto; background: var(--panel); border: 1px solid var(--border); border-radius: 6px; min-height: 0; }
table { width: 100%; border-collapse: collapse; }
th, td { text-align: left; padding: 4px 8px; border-bottom: 1px solid #eef0f3; white-space: nowrap; }
th { position: sticky; top: 0; background: #f8fafc; font-weight: 600; }
td.reason { white-space: normal; color: var(--ng); }
tr.swap td { background: #fffbeb; color: #b45309; font-size: 12px; }
</style>
