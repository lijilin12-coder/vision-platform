<script setup lang="ts">
import { Handle, Position } from "@vue-flow/core";
import { computed } from "vue";
import type { NodeRun, OperatorMeta } from "../platform/types";
import { portColors, portTypeNames } from "../useEditor";

// 画布上的一个节点：左边是输入端口，右边是输出端口。端口的颜色表示类型。
const props = defineProps<{
  id: string;
  data: { operator: OperatorMeta | undefined; operatorId: string; run: NodeRun | undefined; selected: boolean };
}>();

const rows = computed(() => Math.max(props.data.operator?.inputs.length ?? 0, props.data.operator?.outputs.length ?? 0, 1));
const top = (index: number) => `${46 + index * 22}px`;
</script>

<template>
  <div class="op-node" :class="[data.run?.status, { selected: data.selected }]" :style="{ height: `${40 + rows * 22 + 8}px` }" :data-node="id">
    <div class="title">
      <span class="name">{{ data.operator?.displayName ?? data.operatorId }}</span>
      <span class="id">{{ id }}</span>
    </div>
    <div class="time" v-if="data.run">
      <template v-if="data.run.status === 'succeeded'">{{ data.run.durationMs.toFixed(2) }} ms</template>
      <template v-else-if="data.run.status === 'failed'">失败</template>
      <template v-else>跳过</template>
    </div>
    <template v-for="(p, i) in data.operator?.inputs ?? []" :key="'in-' + p.name">
      <Handle :id="p.name" type="target" :position="Position.Left" :style="{ top: top(i), background: portColors[p.type] }" :title="portTypeNames[p.type]" />
      <span class="port in" :style="{ top: top(i) }">{{ p.name }}</span>
    </template>
    <template v-for="(p, i) in data.operator?.outputs ?? []" :key="'out-' + p.name">
      <Handle :id="p.name" type="source" :position="Position.Right" :style="{ top: top(i), background: portColors[p.type] }" :title="portTypeNames[p.type]" />
      <span class="port out" :style="{ top: top(i) }">{{ p.name }}</span>
    </template>
  </div>
</template>

<style scoped>
.op-node {
  position: relative; width: 170px; background: #fff; border: 1.5px solid #c5cad3; border-radius: 6px; box-shadow: 0 1px 2px rgba(0, 0, 0, 0.06);
}
.op-node.selected { border-color: var(--accent); box-shadow: 0 0 0 3px #bfdbfe; }
.op-node.succeeded { border-left: 5px solid var(--ok); }
.op-node.failed { border-left: 5px solid var(--ng); background: #fef2f2; }
.op-node.skipped { border-left: 5px solid #9ca3af; opacity: 0.75; }
.title { display: flex; justify-content: space-between; align-items: baseline; padding: 6px 8px 0; }
.name { font-weight: 600; }
.id { color: var(--muted); font-size: 11px; }
.time { position: absolute; right: 8px; top: 22px; font-size: 10px; color: var(--muted); }
.port { position: absolute; font-size: 11px; color: var(--muted); transform: translateY(-50%); pointer-events: none; }
.port.in { left: 10px; }
.port.out { right: 10px; }
:deep(.vue-flow__handle) { width: 10px; height: 10px; border: 1.5px solid #fff; }
</style>
