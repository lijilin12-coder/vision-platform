<script setup lang="ts">
import { computed, inject, onMounted, reactive, ref } from "vue";
import { acceptanceLabel, percent, releaseTitle, verdictLine, wrongKind } from "../releases";
import type { Releases } from "../releases";

// 上线包页面：上线包的列表和验收状态；建基准图集；给上线包做验收检查，看报告。
const releases = inject<Releases>("releases")!;
const state = releases.state;
const selected = computed(() => releases.selected());
const acceptance = computed(() => selected.value?.acceptance ?? null);
const form = reactive({ name: "", okDirectory: "", ngDirectory: "", maxMisses: 0, falseAlarmPercent: 0, maxErrors: 0 });

onMounted(() => releases.refresh());

async function createGoldenSet() {
  const ok = await releases.createGoldenSet({
    name: form.name.trim(), okDirectory: form.okDirectory.trim(), ngDirectory: form.ngDirectory.trim(),
    maxMisses: Number(form.maxMisses) || 0, maxFalseAlarmRate: (Number(form.falseAlarmPercent) || 0) / 100, maxErrors: Number(form.maxErrors) || 0,
  });
  if (ok) form.name = "";
}
const exportPath = ref("");
const time = (ms: number) => new Date(ms).toLocaleString();
</script>

<template>
  <div class="page">
    <aside class="list">
      <h3>上线包</h3>
      <div v-for="r in state.releases" :key="r.id" class="item" :class="{ active: r.id === state.selectedId }" :data-release="r.id" @click="releases.select(r.id)">
        <span>{{ releaseTitle(r) }}</span>
        <span class="tag" :class="r.withdrawn ? 'withdrawn' : r.acceptanceState" :data-release-state="r.acceptanceState">{{ acceptanceLabel(r) }}</span>
      </div>
      <p v-if="state.releases.length === 0" class="muted">还没有上线包：在“方案编辑”里发布方案，再点“做成上线包”。</p>
    </aside>

    <section class="detail">
      <div v-if="state.error" class="message" data-release-error @click="releases.dismiss()">{{ state.error }} <span class="close">点一下关闭</span></div>

      <div v-if="state.notice" class="message ok" data-release-notice @click="releases.dismiss()">{{ state.notice }} <span class="close">点一下关闭</span></div>

      <div class="box">
        <h4>基准图集</h4>
        <p class="muted">一组带着“真实判定”的图像：哪些是合格的、哪些是不合格的，由人确认。验收检查拿它当标准答案。</p>
        <div v-for="g in state.goldenSets" :key="g.id" class="row" :data-golden="g.name">
          <strong>{{ g.name }}</strong>
          <span>{{ g.cases }} 个用例（合格 {{ g.truthOk }}，不合格 {{ g.truthNg }}）</span>
          <span class="muted">验收标准：漏检 ≤ {{ g.criteria.maxMisses }}，误报率 ≤ {{ percent(g.criteria.maxFalseAlarmRate) }}，出错 ≤ {{ g.criteria.maxErrors }}</span>
        </div>
        <div class="row form">
          <input v-model="form.name" placeholder="名称" data-golden-name class="short" />
          <input v-model="form.okDirectory" placeholder="合格图像的目录（后端那台机器上）" data-golden-ok />
          <input v-model="form.ngDirectory" placeholder="不合格图像的目录" data-golden-ng />
        </div>
        <div class="row form">
          <label>最多漏检 <input v-model="form.maxMisses" type="number" min="0" class="num" data-golden-misses /> 张</label>
          <label>误报率不超过 <input v-model="form.falseAlarmPercent" type="number" min="0" max="100" step="0.5" class="num" /> %</label>
          <label>最多出错 <input v-model="form.maxErrors" type="number" min="0" class="num" /> 张</label>
          <button :disabled="!form.name.trim() || (!form.okDirectory.trim() && !form.ngDirectory.trim())" data-golden-new @click="createGoldenSet">新建基准图集</button>
        </div>
        <p class="muted">建好之后不能再改（改标准或图像 = 新建一个）。放进基准图集的图像不能再拿去训练模型。</p>
      </div>

      <div class="box" v-if="selected">
        <h4>验收检查：{{ releaseTitle(selected) }}</h4>
        <div class="row">
          <select v-model="state.goldenSetId" data-accept-golden>
            <option value="" disabled>选一个基准图集…</option>
            <option v-for="g in state.goldenSets" :key="g.id" :value="g.id">{{ g.name }}（{{ g.cases }} 个用例，漏检 ≤ {{ g.criteria.maxMisses }}）</option>
          </select>
          <button class="primary" :disabled="!state.goldenSetId || state.checking || selected.withdrawn" data-accept @click="releases.check()">
            {{ state.checking ? "正在检查…" : acceptance ? "重新验收" : "验收" }}
          </button>
          <span class="muted" v-if="selected.withdrawn">已撤回的上线包不再验收</span>
        </div>

        <template v-if="acceptance">
          <div class="verdict" :class="acceptance.passed ? 'passed' : 'failed'" data-accept-verdict>{{ verdictLine(acceptance) }}</div>
          <div class="muted">基准图集“{{ acceptance.goldenSetName }}” · {{ time(acceptance.checkedAtMs) }} · 平均 {{ acceptance.meanMs.toFixed(1) }} ms，最慢 {{ acceptance.maxMs.toFixed(1) }} ms</div>
          <table>
            <thead><tr><th></th><th>结果</th><th>验收标准</th></tr></thead>
            <tbody>
              <tr><td>漏检（不合格的判成了合格）</td><td :class="{ over: acceptance.misses > acceptance.criteria.maxMisses }">{{ acceptance.misses }} / {{ acceptance.truthNg }} 张</td><td>≤ {{ acceptance.criteria.maxMisses }} 张</td></tr>
              <tr><td>误报（合格的判成了不合格）</td><td :class="{ over: acceptance.falseAlarmRate > acceptance.criteria.maxFalseAlarmRate + 1e-12 }">{{ acceptance.falseAlarms }} / {{ acceptance.truthOk }} 张，{{ percent(acceptance.falseAlarmRate) }}</td><td>≤ {{ percent(acceptance.criteria.maxFalseAlarmRate) }}</td></tr>
              <tr><td>出错（检测没做下来）</td><td :class="{ over: acceptance.errors > acceptance.criteria.maxErrors }">{{ acceptance.errors }} 张</td><td>≤ {{ acceptance.criteria.maxErrors }} 张</td></tr>
            </tbody>
          </table>
          <div v-if="acceptance.wrongCases.length > 0">
            <strong>判错的用例</strong>
            <div v-for="w in acceptance.wrongCases" :key="w.caseId" class="wrong" data-wrong-case>
              <span class="kind">{{ wrongKind(w) }}</span> {{ w.caseId }} <span class="muted">真实 {{ w.truth }}，判成 {{ w.observed }} · {{ w.imageRef }}</span>
            </div>
          </div>
          <div class="row" v-if="acceptance.passed && !selected.withdrawn">
            <span>带到产线：</span>
            <input v-model="exportPath" placeholder="导出到哪个文件（后端那台机器上），例如 ./亮斑.vrel" data-export-path class="path" />
            <button :disabled="!exportPath.trim()" data-export @click="releases.exportTo(exportPath)">导出上线包文件</button>
          </div>
          <p class="muted" v-if="!acceptance.passed">验收没有通过的上线包不能部署到工位，也不能导出。改方案、重新发布、做成新的上线包再验收；或者——如果是标准定得不对——新建一个基准图集重新验收。</p>
        </template>
        <p v-else class="muted" data-accept-none>还没有做过验收检查。只有验收通过的上线包才能部署到工位、才能导出带到产线。</p>
      </div>
      <p v-else class="muted">在左边选一个上线包。</p>
    </section>
  </div>
</template>

<style scoped>
.page { display: grid; grid-template-columns: 300px 1fr; height: 100%; min-height: 0; }
.list { background: var(--panel); border-right: 1px solid var(--border); padding: 10px; overflow-y: auto; }
.item { display: flex; justify-content: space-between; align-items: center; gap: 6px; padding: 6px 8px; border-radius: 4px; cursor: pointer; }
.item:hover, .item.active { background: #eff6ff; }
.tag { font-size: 12px; padding: 1px 6px; border-radius: 10px; background: #e5e7eb; white-space: nowrap; }
.tag.passed { background: #dcfce7; color: #15803d; }
.tag.failed { background: #fee2e2; color: #b91c1c; }
.tag.withdrawn { background: #e5e7eb; color: #6b7280; text-decoration: line-through; }
.detail { padding: 12px 16px; display: flex; flex-direction: column; gap: 12px; overflow-y: auto; min-width: 0; }
.message { background: #fef2f2; color: #b91c1c; border: 1px solid #fecaca; border-radius: 4px; padding: 6px 10px; cursor: pointer; }
.message.ok { background: #f0fdf4; color: #15803d; border-color: #bbf7d0; }
.row input.path { flex: 1; max-width: 460px; }
.close { float: right; opacity: 0.6; font-size: 11px; }
.box { background: var(--panel); border: 1px solid var(--border); border-radius: 6px; padding: 10px 14px; display: flex; flex-direction: column; gap: 8px; }
h4 { margin: 0; }
p { margin: 0; }
.row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.form input { flex: 1; min-width: 160px; }
.form input.short { flex: 0 0 140px; min-width: 0; }
.form input.num { flex: 0 0 64px; min-width: 0; width: 64px; }
.verdict { font-weight: 600; padding: 6px 10px; border-radius: 4px; }
.verdict.passed { background: #dcfce7; color: #15803d; }
.verdict.failed { background: #fee2e2; color: #b91c1c; }
table { border-collapse: collapse; max-width: 640px; }
th, td { text-align: left; padding: 4px 14px 4px 0; border-bottom: 1px solid #eef0f3; }
td.over { color: var(--ng); font-weight: 600; }
.wrong { padding: 2px 0; }
.kind { display: inline-block; min-width: 2.5em; color: var(--ng); font-weight: 600; }
</style>
