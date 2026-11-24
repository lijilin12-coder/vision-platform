<script setup lang="ts">
import { computed } from "vue";
import type { OperatorMeta } from "../platform/types";
import { useEditor } from "../useEditor";

// 算子面板：按分类列出后端有的全部算子。这个列表完全来自后端——后端新增一个算子，这里自己就多一项。
const editor = useEditor();

const order = ["输入", "入口", "预处理", "分割", "分析", "测量", "定位", "深度学习", "判定"];
const groups = computed(() => {
  const map = new Map<string, OperatorMeta[]>();
  for (const op of editor.state.operators) {
    if (!map.has(op.category)) map.set(op.category, []);
    map.get(op.category)!.push(op);
  }
  return [...map.entries()].sort((a, b) => {
    const ia = order.indexOf(a[0]);
    const ib = order.indexOf(b[0]);
    return (ia < 0 ? 99 : ia) - (ib < 0 ? 99 : ib);
  });
});

function onDragStart(event: DragEvent, operatorId: string) {
  event.dataTransfer?.setData("application/x-vision-operator", operatorId);
}
</script>

<template>
  <div class="palette">
    <h3>算子</h3>
    <div v-for="[category, ops] in groups" :key="category" class="group">
      <div class="category">{{ category }}</div>
      <div
        v-for="op in ops"
        :key="op.id"
        class="operator"
        :class="{ disabled: !editor.state.recipe }"
        :title="op.description"
        :data-operator="op.id"
        :draggable="!!editor.state.recipe"
        @dragstart="onDragStart($event, op.id)"
        @click="editor.state.recipe && editor.addNode(op.id)"
      >
        {{ op.displayName }}
      </div>
    </div>
  </div>
</template>

<style scoped>
.palette { padding: 10px; overflow-y: auto; height: 100%; }
.category { margin: 8px 0 3px; font-size: 11px; color: var(--muted); }
.operator { padding: 4px 8px; margin-bottom: 3px; border: 1px solid var(--border); border-radius: 4px; background: #fff; cursor: grab; user-select: none; }
.operator:hover { border-color: var(--accent); color: var(--accent); }
.operator.disabled { opacity: 0.5; cursor: default; }
</style>
