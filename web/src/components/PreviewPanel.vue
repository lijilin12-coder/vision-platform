<script setup lang="ts">
import { computed } from "vue";
import { useEditor } from "../useEditor";

// 预览：选中节点的输出图像；没有选中时是送进去的原图。上面叠着这次检测发现的缺陷（框）和定位到的位置（十字）。
// 右边是这次运行的结论：判定、判定依据、结果项。
const editor = useEditor();
const state = editor.state;

const shown = computed(() => {
  const run = state.run;
  if (!run) return state.image ? { url: editor.imageUrl(state.image.imageId), label: "原图", nodeId: null } : null;
  const node = run.nodes.find((n) => n.nodeId === state.selectedNodeId);
  if (node) {
    const ports = Object.entries(node.outputs).filter(([, o]) => o.hasImage);
    const chosen = ports.find(([name]) => name === state.selectedPort) ?? ports[0];
    if (chosen) return { url: editor.outputUrl(chosen[1].imageUrl!), label: `${node.nodeId}.${chosen[0]}`, nodeId: node.nodeId };
  }
  return state.image ? { url: editor.imageUrl(state.image.imageId), label: "原图", nodeId: null } : null;
});

const defects = computed(() => state.run?.result.defects ?? []);
const poses = computed(() => (state.run?.result.poses ?? []).filter((p) => p.found));
</script>

<template>
  <div class="preview">
    <div class="image-area">
      <template v-if="shown && state.image">
        <div class="caption">
          {{ shown.label }}
          <span class="muted" v-if="state.lastRunMs !== null">· 改动到预览更新 {{ state.lastRunMs }} ms</span>
          <span class="muted" v-if="state.running">· 运行中…</span>
        </div>
        <div class="frame">
          <img :src="shown.url" :alt="shown.label" data-preview />
          <svg :viewBox="`0 0 ${state.image.width} ${state.image.height}`" preserveAspectRatio="none">
            <rect v-for="(d, i) in defects" :key="'d' + i" :x="d.left - 2" :y="d.top - 2" :width="d.width + 4" :height="d.height + 4" class="defect">
              <title>{{ d.category }}：面积 {{ d.area }}</title>
            </rect>
            <g v-for="(p, i) in poses" :key="'p' + i" class="pose">
              <line :x1="p.x - 14" :y1="p.y" :x2="p.x + 14" :y2="p.y" />
              <line :x1="p.x" :y1="p.y - 14" :x2="p.x" :y2="p.y + 14" />
            </g>
          </svg>
        </div>
      </template>
      <div v-else class="empty muted">选一幅图像（上面的“选择图像”），就能看到每个节点的结果</div>
    </div>

    <div class="result" v-if="state.run">
      <div class="verdict">
        <span class="badge" :class="state.run.verdict" data-verdict>{{ state.run.verdict }}</span>
        <span class="muted">检测耗时 {{ state.run.totalMs.toFixed(2) }} ms</span>
      </div>
      <div v-for="(r, i) in state.run.reasons" :key="'r' + i" :class="r.pass ? 'pass' : 'fail'">{{ r.pass ? "✓" : "✗" }} {{ r.nodeId }}：{{ r.text }}</div>
      <template v-if="defects.length > 0">
        <div class="section-title">缺陷 {{ defects.length }} 个</div>
        <div v-for="(d, i) in defects.slice(0, 8)" :key="'dl' + i" class="item">
          {{ d.category }} · ({{ d.centerX.toFixed(0) }}, {{ d.centerY.toFixed(0) }}) · {{ d.width }}×{{ d.height }} · 面积 {{ d.area.toFixed(0) }}
        </div>
        <div v-if="defects.length > 8" class="muted">……还有 {{ defects.length - 8 }} 个</div>
      </template>
      <template v-if="state.run.result.measurements.length > 0">
        <div class="section-title">测量值</div>
        <div v-for="(m, i) in state.run.result.measurements" :key="'m' + i" class="item" :class="{ fail: !m.pass }">
          {{ m.name }} = {{ m.value.toFixed(2) }} {{ m.unit }}
          <span class="muted" v-if="m.lower !== undefined">（允许 {{ m.lower }} ~ {{ m.upper }}）</span>
        </div>
      </template>
      <template v-if="state.run.result.poses.length > 0">
        <div class="section-title">定位</div>
        <div v-for="(p, i) in state.run.result.poses" :key="'pl' + i" class="item" :class="{ fail: !p.found }">
          <template v-if="p.found">({{ p.x.toFixed(1) }}, {{ p.y.toFixed(1) }}) 旋转 {{ p.angle.toFixed(1) }}° · 相似度 {{ (p.score * 100).toFixed(0) }}%</template>
          <template v-else>没有找到（最好的相似度 {{ (p.score * 100).toFixed(0) }}%）</template>
        </div>
      </template>
    </div>
  </div>
</template>

<style scoped>
.preview { display: grid; grid-template-columns: 1fr 300px; height: 100%; min-height: 0; }
.image-area { display: flex; flex-direction: column; min-width: 0; min-height: 0; padding: 8px 10px; }
.caption { margin-bottom: 4px; font-weight: 600; }
.caption .muted { font-weight: 400; }
.frame { position: relative; align-self: flex-start; max-width: 100%; min-height: 0; flex: 0 1 auto; display: flex; }
.frame img { display: block; max-width: 100%; max-height: 100%; object-fit: contain; background: #000; }
.frame svg { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
.defect { fill: none; stroke: #ff3b30; stroke-width: 2; vector-effect: non-scaling-stroke; pointer-events: all; }
.pose line { stroke: #22d3ee; stroke-width: 2; vector-effect: non-scaling-stroke; }
.empty { flex: 1; display: flex; align-items: center; justify-content: center; }
.result { border-left: 1px solid var(--border); padding: 8px 10px; overflow-y: auto; line-height: 1.6; }
.verdict { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.verdict .badge { font-size: 15px; padding: 2px 12px; }
.pass { color: var(--ok); }
.fail { color: var(--ng); }
.section-title { font-weight: 600; margin-top: 8px; }
.item { font-size: 12px; }
</style>
