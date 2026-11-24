<script setup lang="ts">
import { onMounted, ref } from "vue";
import FlowCanvas from "./components/FlowCanvas.vue";
import OperatorPalette from "./components/OperatorPalette.vue";
import ParamPanel from "./components/ParamPanel.vue";
import PreviewPanel from "./components/PreviewPanel.vue";
import { useEditor } from "./useEditor";

const editor = useEditor();
const state = editor.state;
const fileInput = ref<HTMLInputElement | null>(null);
const newName = ref("");

onMounted(() => editor.init());

function onFile(event: Event) {
  const file = (event.target as HTMLInputElement).files?.[0];
  if (file) void editor.uploadImage(file);
  (event.target as HTMLInputElement).value = ""; // 同一个文件再选一次也要触发
}

async function create() {
  const name = newName.value.trim();
  if (!name) return;
  await editor.newRecipe(name);
  newName.value = "";
}
</script>

<template>
  <div class="app">
    <header class="top">
      <strong class="brand">视觉检测平台 · 方案编辑</strong>
      <select :value="state.recipe?.id ?? ''" data-recipe-select @change="editor.openRecipe(($event.target as HTMLSelectElement).value)">
        <option value="" disabled>打开方案…</option>
        <option v-for="r in state.recipes" :key="r.id" :value="r.id">{{ r.name }}</option>
      </select>
      <input v-model="newName" placeholder="新方案的名称" data-new-name @keyup.enter="create" />
      <button :disabled="!newName.trim()" data-new @click="create">新建</button>
      <span class="spacer" />
      <input ref="fileInput" type="file" accept="image/*" hidden data-file @change="onFile" />
      <button :disabled="!state.recipe" @click="fileInput?.click()">选择图像</button>
      <button :disabled="!state.recipe || !state.image || state.running" data-run @click="editor.run()">运行</button>
      <span v-if="state.run" class="badge" :class="state.run.verdict">{{ state.run.verdict }}</span>
      <button class="primary" :disabled="!state.recipe" data-publish @click="editor.publish()">发布</button>
    </header>

    <div v-if="state.error" class="message error" data-error @click="editor.dismiss()">{{ state.error }} <span class="close">点一下关闭</span></div>
    <div v-else-if="state.notice" class="message notice" data-notice @click="editor.dismiss()">{{ state.notice }} <span class="close">点一下关闭</span></div>

    <aside class="left"><OperatorPalette /></aside>
    <main class="center"><FlowCanvas /></main>
    <aside class="right"><ParamPanel /></aside>
    <section class="bottom"><PreviewPanel /></section>
  </div>
</template>

<style scoped>
.app {
  display: grid; height: 100%;
  grid-template-columns: 170px 1fr 290px;
  grid-template-rows: auto auto minmax(0, 1fr) 42%;
  grid-template-areas: "top top top" "message message message" "left center right" "left bottom right";
}
.top { grid-area: top; display: flex; align-items: center; gap: 8px; padding: 8px 12px; background: var(--panel); border-bottom: 1px solid var(--border); }
.brand { margin-right: 8px; }
.spacer { flex: 1; }
.message { grid-area: message; padding: 6px 12px; cursor: pointer; }
.message.error { background: #fef2f2; color: #b91c1c; border-bottom: 1px solid #fecaca; }
.message.notice { background: #f0fdf4; color: #15803d; border-bottom: 1px solid #bbf7d0; }
.close { float: right; opacity: 0.6; font-size: 11px; }
.left { grid-area: left; background: var(--panel); border-right: 1px solid var(--border); min-height: 0; }
.center { grid-area: center; min-height: 0; min-width: 0; }
.right { grid-area: right; background: var(--panel); border-left: 1px solid var(--border); min-height: 0; }
.bottom { grid-area: bottom; background: var(--panel); border-top: 1px solid var(--border); min-height: 0; min-width: 0; }
</style>
