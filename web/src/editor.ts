import { reactive } from "vue";
import type { PlatformClient } from "./platform/PlatformClient";
import type { DebugRun, ModelInfo, OperatorMeta, ParamValue, PortDef, Recipe, RecipeSummary, Roi, UploadedImage } from "./platform/types";

// 方案编辑器的状态和操作。组件只做显示和把用户的动作交给这里；和后端打交道的事都在这里，通过 PlatformClient。
//
// 两条原则：
//   1. 方案以后端为准。每个编辑操作都是“请后端改 → 用后端回复的最新方案替换本地的”。
//      后端拒绝（规则不允许）时本地什么都不变，把后端给的原因显示出来。界面不自己判断规则。
//   2. 改了任何东西之后，如果已经选了图像，就自动重新运行一次：改参数、看结果是一个连续的动作。

export interface Position {
  x: number;
  y: number;
}

export interface EditorState {
  operators: OperatorMeta[];
  models: ModelInfo[];
  recipes: RecipeSummary[];
  recipe: Recipe | null;
  positions: Record<string, Position>; // 节点在画布上的位置。只存在浏览器里，不属于方案
  selectedNodeId: string | null;
  selectedPort: string | null; // 预览哪个输出；空表示自动挑一个
  image: UploadedImage | null;
  run: DebugRun | null;
  running: boolean;
  lastRunMs: number | null; // 最近一次“改动 → 预览更新”花了多久
  error: string | null;
  notice: string | null;
  publishedVersionId: string | null;
}

export interface Storage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}

export function createEditor(client: PlatformClient, options: { autoRunDelayMs?: number; storage?: Storage } = {}) {
  const delay = options.autoRunDelayMs ?? 200;
  const storage = options.storage;
  const state = reactive<EditorState>({
    operators: [],
    models: [],
    recipes: [],
    recipe: null,
    positions: {},
    selectedNodeId: null,
    selectedPort: null,
    image: null,
    run: null,
    running: false,
    lastRunMs: null,
    error: null,
    notice: null,
    publishedVersionId: null,
  });

  let timer: ReturnType<typeof setTimeout> | null = null;
  let runToken = 0;
  let pending: Promise<void> = Promise.resolve();

  function fail(e: unknown) {
    state.error = e instanceof Error ? e.message : String(e);
  }

  // 做一个可能被后端拒绝的操作。成功返回 true
  async function attempt(action: () => Promise<void>): Promise<boolean> {
    state.error = null;
    try {
      await action();
      return true;
    } catch (e) {
      fail(e);
      return false;
    }
  }

  function operatorOf(nodeId: string): OperatorMeta | undefined {
    const node = state.recipe?.nodes.find((n) => n.id === nodeId);
    return node ? state.operators.find((o) => o.id === node.operatorId) : undefined;
  }

  function port(ref: string, side: "inputs" | "outputs"): PortDef | undefined {
    const dot = ref.indexOf(".");
    return operatorOf(ref.slice(0, dot))?.[side].find((p) => p.name === ref.slice(dot + 1));
  }

  // ---- 节点的位置 ----

  function loadPositions(recipeId: string) {
    try {
      state.positions = JSON.parse(storage?.getItem(`vision.positions.${recipeId}`) ?? "{}");
    } catch {
      state.positions = {};
    }
  }

  function savePositions() {
    if (state.recipe) storage?.setItem(`vision.positions.${state.recipe.id}`, JSON.stringify(state.positions));
  }

  // 没有位置的节点自动排：按连线关系从左到右一列一列
  function layoutMissing() {
    const recipe = state.recipe;
    if (!recipe) return;
    const depth: Record<string, number> = {};
    const depthOf = (id: string, seen: Set<string> = new Set()): number => {
      if (depth[id] !== undefined) return depth[id];
      if (seen.has(id)) return 0;
      seen.add(id);
      const upstream = recipe.edges.filter((e) => e.to.startsWith(id + ".")).map((e) => e.from.slice(0, e.from.indexOf(".")));
      depth[id] = upstream.length === 0 ? 0 : 1 + Math.max(...upstream.map((u) => depthOf(u, seen)));
      return depth[id];
    };
    const rows: Record<number, number> = {};
    for (const node of recipe.nodes) {
      const d = depthOf(node.id);
      const row = rows[d] ?? 0;
      rows[d] = row + 1;
      if (!state.positions[node.id]) state.positions[node.id] = { x: 40 + d * 250, y: 40 + row * 150 };
    }
    for (const id of Object.keys(state.positions)) {
      if (!recipe.nodes.some((n) => n.id === id)) delete state.positions[id];
    }
  }

  function adopt(recipe: Recipe) {
    state.recipe = recipe;
    state.publishedVersionId = null;
    if (state.selectedNodeId && !recipe.nodes.some((n) => n.id === state.selectedNodeId)) state.selectedNodeId = null;
    layoutMissing();
    savePositions();
  }

  // ---- 运行 ----

  async function runNow(startedAt = Date.now()): Promise<void> {
    if (!state.recipe || !state.image) return;
    const token = ++runToken;
    state.running = true;
    try {
      const run = await client.debugRun(state.recipe.id, state.image.imageId);
      if (token !== runToken) return; // 这期间又改过了：这个结果已经过时，丢掉
      state.run = run;
      state.lastRunMs = Date.now() - startedAt;
    } catch (e) {
      if (token === runToken) {
        state.run = null;
        fail(e);
      }
    } finally {
      if (token === runToken) state.running = false;
    }
  }

  function scheduleRun() {
    if (!state.recipe || !state.image) return;
    const startedAt = Date.now();
    if (timer) clearTimeout(timer);
    let done: () => void = () => {};
    pending = new Promise<void>((resolve) => (done = resolve));
    timer = setTimeout(() => {
      timer = null;
      void runNow(startedAt).finally(done);
    }, delay);
  }

  // 做一个修改方案的操作：成功后采用后端给的新方案，并安排重新运行
  async function edit(action: () => Promise<Recipe>): Promise<boolean> {
    return attempt(async () => {
      adopt(await action());
      scheduleRun();
    });
  }

  return {
    state,
    operatorOf,

    // 自动运行是延迟的；测试里用它等到“该跑的都跑完了”
    async settled() {
      await pending;
    },

    async init() {
      await attempt(async () => {
        [state.operators, state.models, state.recipes] = await Promise.all([client.listOperators(), client.listModels(), client.listRecipes()]);
      });
    },

    async openRecipe(recipeId: string) {
      await attempt(async () => {
        const recipe = await client.getRecipe(recipeId);
        loadPositions(recipe.id);
        state.selectedNodeId = null;
        state.selectedPort = null;
        state.run = null;
        adopt(recipe);
        scheduleRun();
      });
    },

    async newRecipe(name: string) {
      await attempt(async () => {
        const recipe = await client.createRecipe(name);
        state.positions = {};
        state.selectedNodeId = null;
        state.run = null;
        adopt(recipe);
        state.recipes = await client.listRecipes();
      });
    },

    async addNode(operatorId: string, position?: Position) {
      if (!state.recipe) return false;
      const recipeId = state.recipe.id;
      return attempt(async () => {
        // 没有指定位置（点了一下算子，而不是拖到画布上）：放在最右边那个节点的右侧，方案自然地从左往右长
        const existing = Object.values(state.positions);
        const rightmost = existing.reduce<Position | null>((best, p) => (best === null || p.x > best.x ? p : best), null);
        const { recipe, nodeId } = await client.addNode(recipeId, operatorId);
        state.positions[nodeId] = position ?? (rightmost ? { x: rightmost.x + 210, y: rightmost.y } : { x: 40, y: 60 });
        adopt(recipe);
        state.selectedNodeId = nodeId;
        state.selectedPort = null;
        scheduleRun();
      });
    },

    removeNode(nodeId: string) {
      const recipeId = state.recipe?.id;
      return recipeId ? edit(() => client.removeNode(recipeId, nodeId)) : Promise.resolve(false);
    },

    // 两个端口能不能连：只是给界面提示用的（拖线时高亮）。真正的检查在后端
    canConnect(from: string, to: string): boolean {
      const a = port(from, "outputs");
      const b = port(to, "inputs");
      return !!a && !!b && a.type === b.type;
    },

    connect(from: string, to: string) {
      const recipeId = state.recipe?.id;
      return recipeId ? edit(() => client.connect(recipeId, from, to)) : Promise.resolve(false);
    },

    disconnect(from: string, to: string) {
      const recipeId = state.recipe?.id;
      return recipeId ? edit(() => client.disconnect(recipeId, from, to)) : Promise.resolve(false);
    },

    setParam(nodeId: string, name: string, value: ParamValue) {
      const recipeId = state.recipe?.id;
      return recipeId ? edit(() => client.setParam(recipeId, nodeId, name, value)) : Promise.resolve(false);
    },

    setRoi(nodeId: string, roi: Roi | null) {
      const recipeId = state.recipe?.id;
      return recipeId ? edit(() => client.setRoi(recipeId, nodeId, roi)) : Promise.resolve(false);
    },

    moveNode(nodeId: string, position: Position) {
      state.positions[nodeId] = position;
      savePositions();
    },

    select(nodeId: string | null, portName: string | null = null) {
      state.selectedNodeId = nodeId;
      state.selectedPort = portName;
    },

    async uploadImage(file: Blob) {
      await attempt(async () => {
        state.image = await client.uploadImage(file);
        state.run = null;
        await runNow();
      });
    },

    run: () => runNow(),

    async publish() {
      const recipeId = state.recipe?.id;
      if (!recipeId) return;
      await attempt(async () => {
        state.publishedVersionId = await client.publish(recipeId);
        state.notice = `已发布：${state.publishedVersionId}`;
      });
    },

    // 把刚发布的方案版本做成上线包（之后就可以在“工位”页面里部署）
    async createRelease() {
      const versionId = state.publishedVersionId;
      if (!versionId) return;
      await attempt(async () => {
        const releaseId = await client.createRelease(versionId);
        state.publishedVersionId = null;
        state.notice = `已做成上线包 ${releaseId.slice(0, 8)}。到“上线包”页面里验收，再到“工位”页面里部署`;
      });
    },

    dismiss() {
      state.error = null;
      state.notice = null;
    },

    imageUrl: (imageId: string) => client.imageUrl(imageId),
    outputUrl: (relativeUrl: string) => client.outputUrl(relativeUrl),
  };
}

export type Editor = ReturnType<typeof createEditor>;
