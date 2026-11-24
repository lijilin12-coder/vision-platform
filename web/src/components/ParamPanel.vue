<script setup lang="ts">
import { computed, reactive, watch } from "vue";
import type { ParamDef, ParamValue } from "../platform/types";
import { portTypeNames, useEditor } from "../useEditor";

// 参数面板：选中节点的参数、ROI、输出。
// 参数的输入框是按算子的参数定义【自动生成】的：数字 → 数字框，选项 → 下拉框，布尔 → 勾选框……
// 所以后端新增一个算子，这里不用写任何代码。
const editor = useEditor();
const state = editor.state;

const node = computed(() => state.recipe?.nodes.find((n) => n.id === state.selectedNodeId) ?? null);
const operator = computed(() => (node.value ? editor.operatorOf(node.value.id) : undefined));
const run = computed(() => state.run?.nodes.find((r) => r.nodeId === state.selectedNodeId));

// ROI 的四个输入框：填完点“应用”才交给后端
const roi = reactive({ x: 0, y: 0, width: 0, height: 0 });
// 节点没有 ROI 时，输入框里先放上整幅图像的范围，改起来方便
watch(
  [node, () => state.image],
  ([n]) => Object.assign(roi, n?.roi ?? { x: 0, y: 0, width: state.image?.width ?? 0, height: state.image?.height ?? 0 }),
  { immediate: true },
);

function setNumber(def: ParamDef, text: string) {
  const value = Number(text);
  if (text.trim() === "" || Number.isNaN(value)) return;
  void editor.setParam(node.value!.id, def.name, value);
}
function set(def: ParamDef, value: ParamValue) {
  void editor.setParam(node.value!.id, def.name, value);
}
function describe(v: unknown): string {
  if (typeof v === "number") return Number.isInteger(v) ? String(v) : v.toFixed(3);
  if (typeof v === "boolean") return v ? "是" : "否";
  if (typeof v === "string") return v;
  if (Array.isArray(v)) return `${v.length} 个`;
  if (v && typeof v === "object") {
    const o = v as Record<string, unknown>;
    if ("pass" in o) return `${o.pass ? "合格" : "不合格"}：${o.reason}`;
    if ("angle" in o) return `(${Number(o.x).toFixed(1)}, ${Number(o.y).toFixed(1)}) 旋转 ${Number(o.angle).toFixed(1)}°`;
  }
  return "";
}
</script>

<template>
  <div class="panel">
    <template v-if="node && operator">
      <h3>{{ operator.displayName }} <span class="muted">{{ node.id }}</span></h3>
      <p class="muted description">{{ operator.description }}</p>

      <div class="section" v-if="operator.params.length > 0">
        <div class="section-title">参数</div>
        <label v-for="def in operator.params" :key="def.name" class="param" :data-param="def.name">
          <span class="param-name" :title="def.label">{{ def.name }}</span>
          <input
            v-if="def.type === 'number'"
            type="number"
            :value="node.params[def.name]"
            :min="def.min"
            :max="def.max"
            step="any"
            @change="setNumber(def, ($event.target as HTMLInputElement).value)"
          />
          <input v-else-if="def.type === 'bool'" type="checkbox" :checked="node.params[def.name] === true" @change="set(def, ($event.target as HTMLInputElement).checked)" />
          <select v-else-if="def.type === 'choice'" :value="node.params[def.name]" @change="set(def, ($event.target as HTMLSelectElement).value)">
            <option v-for="c in def.choices" :key="c" :value="c">{{ c }}</option>
          </select>
          <select v-else-if="def.type === 'model'" :value="node.params[def.name]" @change="set(def, ($event.target as HTMLSelectElement).value)">
            <option value="">（没有选）</option>
            <option v-for="m in state.models" :key="m.id" :value="m.id">{{ m.id }}{{ m.status === "published" ? "" : "（未发布）" }}</option>
          </select>
          <select v-else-if="def.type === 'template'" :value="node.params[def.name]" @change="set(def, ($event.target as HTMLSelectElement).value)">
            <option value="">（没有选）</option>
            <option v-for="t in state.recipe?.templates ?? []" :key="t.id" :value="t.id">{{ t.name }}</option>
          </select>
          <input v-else type="text" :value="node.params[def.name]" @change="set(def, ($event.target as HTMLInputElement).value)" />
          <span class="param-label muted">{{ def.label }}</span>
        </label>
      </div>

      <div class="section" v-if="operator.inputs.length > 0">
        <div class="section-title">ROI <span class="muted">{{ node.roi ? "" : "（没有设置：处理整幅图像）" }}</span></div>
        <div class="roi">
          <label>x <input type="number" v-model.number="roi.x" /></label>
          <label>y <input type="number" v-model.number="roi.y" /></label>
          <label>宽 <input type="number" v-model.number="roi.width" /></label>
          <label>高 <input type="number" v-model.number="roi.height" /></label>
        </div>
        <div class="row">
          <button @click="editor.setRoi(node.id, { ...roi })">应用</button>
          <button :disabled="!node.roi" @click="editor.setRoi(node.id, null)">清除</button>
        </div>
      </div>

      <div class="section" v-if="run">
        <div class="section-title">这次运行</div>
        <div v-if="run.status === 'failed'" class="failed">失败：{{ run.message }}</div>
        <div v-else-if="run.status === 'skipped'" class="muted">跳过：{{ run.message }}</div>
        <div
          v-for="(out, port) in run.outputs"
          :key="port"
          class="output"
          :class="{ clickable: out.hasImage, active: state.selectedPort === port }"
          @click="out.hasImage && editor.select(node.id, String(port))"
        >
          <span class="param-name">{{ port }}</span>
          <span class="muted">{{ portTypeNames[out.type] ?? out.type }}</span>
          <span v-if="out.hasImage" class="link">查看</span>
          <span v-else>{{ describe(out.value) }}</span>
          <div v-if="out.statistics" class="statistics muted">{{ out.statistics }}</div>
        </div>
      </div>

      <div class="section">
        <button class="danger" @click="editor.removeNode(node.id)">删除这个节点</button>
      </div>
    </template>
    <p v-else class="muted">点一个节点，在这里改它的参数。</p>
  </div>
</template>

<style scoped>
.panel { padding: 10px; overflow-y: auto; height: 100%; }
.description { margin: 0 0 8px; line-height: 1.5; }
.section { margin-top: 12px; padding-top: 8px; border-top: 1px solid var(--border); }
.section-title { font-weight: 600; margin-bottom: 6px; }
.param { display: grid; grid-template-columns: 78px 1fr; gap: 2px 6px; align-items: center; margin-bottom: 8px; }
.param-name { font-family: ui-monospace, Consolas, monospace; font-size: 12px; overflow: hidden; text-overflow: ellipsis; }
.param-label { grid-column: 1 / 3; font-size: 11px; line-height: 1.4; }
.param input[type="checkbox"] { justify-self: start; }
.roi { display: grid; grid-template-columns: 1fr 1fr; gap: 4px 8px; }
.roi label { display: flex; align-items: center; gap: 4px; }
.roi input { width: 100%; }
.row { display: flex; gap: 6px; margin-top: 6px; }
.output { display: grid; grid-template-columns: 78px 54px 1fr; gap: 2px 6px; padding: 3px 4px; border-radius: 4px; }
.output.clickable { cursor: pointer; }
.output.clickable:hover, .output.active { background: #eff6ff; }
.link { color: var(--accent); }
.statistics { grid-column: 1 / 4; font-size: 11px; line-height: 1.4; }
.failed { color: var(--ng); line-height: 1.5; }
</style>
