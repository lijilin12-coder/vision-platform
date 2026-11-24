<script setup lang="ts">
import { Background } from "@vue-flow/background";
import { Controls } from "@vue-flow/controls";
import { VueFlow, useVueFlow, type Connection, type Edge, type Node, type NodeTypesObject } from "@vue-flow/core";
import { computed, markRaw, nextTick, watch } from "vue";
import { portColors, useEditor } from "../useEditor";
import OperatorNode from "./OperatorNode.vue";

// 画布。它不保存任何自己的状态：节点和连线每次都从编辑器的状态算出来，用户的动作都交给编辑器。
const editor = useEditor();
const state = editor.state;
const { screenToFlowCoordinate, fitView } = useVueFlow();

// 节点的数量变了（加了节点、打开了另一个方案）：调整视野，让所有节点都看得见。只缩小，不放大
watch(
  () => [state.recipe?.id, state.recipe?.nodes.length],
  () => nextTick(() => setTimeout(() => fitView({ maxZoom: 1, padding: 0.12 }), 30)),
);
// 类型上要转一下：Vue Flow 给节点组件传的属性比我们声明要用的多
const nodeTypes = { op: markRaw(OperatorNode) } as unknown as NodeTypesObject;

const nodes = computed<Node[]>(() =>
  (state.recipe?.nodes ?? []).map((n) => ({
    id: n.id,
    type: "op",
    position: state.positions[n.id] ?? { x: 0, y: 0 },
    data: {
      operator: editor.operatorOf(n.id),
      operatorId: n.operatorId,
      run: state.run?.nodes.find((r) => r.nodeId === n.id),
      selected: state.selectedNodeId === n.id,
    },
  })),
);

const edges = computed<Edge[]>(() =>
  (state.recipe?.edges ?? []).map((e) => {
    const [source, sourceHandle] = e.from.split(".");
    const [target, targetHandle] = e.to.split(".");
    const type = editor.operatorOf(source)?.outputs.find((p) => p.name === sourceHandle)?.type ?? "";
    return { id: `${e.from}->${e.to}`, source, sourceHandle, target, targetHandle, style: { stroke: portColors[type] ?? "#9ca3af", strokeWidth: 2 }, data: e };
  }),
);

function onConnect(c: Connection) {
  void editor.connect(`${c.source}.${c.sourceHandle}`, `${c.target}.${c.targetHandle}`);
}

// 拖线时的提示：类型对不上的端口不让放。真正的检查在后端，这里只是让人少走一步冤枉路
function isValidConnection(c: Connection) {
  return editor.canConnect(`${c.source}.${c.sourceHandle}`, `${c.target}.${c.targetHandle}`);
}

function onDrop(event: DragEvent) {
  const operatorId = event.dataTransfer?.getData("application/x-vision-operator");
  if (!operatorId) return;
  const p = screenToFlowCoordinate({ x: event.clientX, y: event.clientY });
  void editor.addNode(operatorId, { x: Math.round(p.x - 85), y: Math.round(p.y - 30) });
}
</script>

<template>
  <div class="canvas" @dragover.prevent @drop="onDrop">
    <VueFlow
      :nodes="nodes"
      :edges="edges"
      :node-types="nodeTypes"
      :is-valid-connection="isValidConnection"
      :delete-key-code="null"
      :min-zoom="0.3"
      :max-zoom="2"
      :default-viewport="{ x: 20, y: 20, zoom: 1 }"
      @connect="onConnect"
      @node-click="(e) => editor.select(e.node.id)"
      @pane-click="() => editor.select(null)"
      @node-drag-stop="(e) => e.nodes.forEach((n) => editor.moveNode(n.id, { x: Math.round(n.position.x), y: Math.round(n.position.y) }))"
      @edge-double-click="(e) => editor.disconnect(e.edge.data.from, e.edge.data.to)"
    >
      <Background :gap="20" />
      <Controls :show-interactive="false" />
    </VueFlow>
    <div class="hint" v-if="state.recipe && state.recipe.nodes.length === 0">从左边把算子拖到这里，或者点一下算子</div>
    <div class="hint" v-if="!state.recipe">先在上面新建或打开一个方案</div>
    <div class="tip" v-if="state.recipe && state.recipe.edges.length > 0">双击连线可以删除它</div>
  </div>
</template>

<style scoped>
.canvas { position: relative; height: 100%; background: #fafbfc; }
.hint { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; color: var(--muted); font-size: 15px; pointer-events: none; }
.tip { position: absolute; right: 10px; bottom: 8px; color: var(--muted); font-size: 11px; pointer-events: none; }
</style>
