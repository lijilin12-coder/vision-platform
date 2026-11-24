import type { PlatformClient } from "./PlatformClient";
import { PlatformError } from "./types";
import type { DebugRun, OperatorMeta, ParamValue, Recipe, Roi } from "./types";

// 测试用的假后端：在内存里记着方案，按很少的几条规则回答。
// 有了它，编辑器的状态和操作可以不启动 vision-runtime、不开浏览器就测。
// （它不是后端规则的复制品：真正的规则只在后端。这里只有测试需要的那一点——类型不对的连线被拒绝。）

const port = (name: string, type: string) => ({ name, type, optional: false });

export const fakeOperators: OperatorMeta[] = [
  { id: "vision.image_input", majorVersion: 1, displayName: "图像入口", category: "入口", description: "", inputs: [],
    outputs: [port("image", "image")], params: [], contributesToVerdict: false },
  { id: "vision.threshold", majorVersion: 1, displayName: "阈值", category: "分割", description: "",
    inputs: [port("image", "image")], outputs: [port("region", "region")],
    params: [{ name: "value", type: "number", default: 128, label: "阈值", min: 0, max: 255 }], contributesToVerdict: false },
  { id: "vision.blob", majorVersion: 1, displayName: "Blob 分析", category: "分析", description: "",
    inputs: [port("region", "region")], outputs: [port("blobs", "blobs"), port("count", "number")], params: [], contributesToVerdict: false },
];

export class FakePlatformClient implements PlatformClient {
  recipes = new Map<string, Recipe>();
  runs = 0; // 调试运行被调用了几次
  runDelayMs = 0;
  failNextRun = false;
  private counter = 0;

  private need(recipeId: string): Recipe {
    const recipe = this.recipes.get(recipeId);
    if (!recipe) throw new PlatformError(`找不到方案 ${recipeId}`, "authoring.recipe_not_found", 404);
    return recipe;
  }
  private copy(recipe: Recipe): Recipe {
    return JSON.parse(JSON.stringify(recipe));
  }
  private typeOf(recipe: Recipe, ref: string, side: "inputs" | "outputs") {
    const [nodeId, name] = ref.split(".");
    const node = recipe.nodes.find((n) => n.id === nodeId);
    return fakeOperators.find((o) => o.id === node?.operatorId)?.[side].find((p) => p.name === name)?.type;
  }

  async listOperators() {
    return fakeOperators;
  }
  async listModels() {
    return [];
  }
  async listRecipes() {
    return [...this.recipes.values()].map((r) => ({ id: r.id, name: r.name }));
  }
  async createRecipe(name: string) {
    const recipe: Recipe = { id: `r${++this.counter}`, name, nodes: [], edges: [], templates: [] };
    this.recipes.set(recipe.id, recipe);
    return this.copy(recipe);
  }
  async getRecipe(recipeId: string) {
    return this.copy(this.need(recipeId));
  }
  async addNode(recipeId: string, operatorId: string) {
    const recipe = this.need(recipeId);
    const op = fakeOperators.find((o) => o.id === operatorId);
    if (!op) throw new PlatformError(`找不到算子 ${operatorId}`, "authoring.operator_not_found", 404);
    const nodeId = `n${recipe.nodes.length + 1}`;
    recipe.nodes.push({ id: nodeId, operatorId, params: Object.fromEntries(op.params.map((p) => [p.name, p.default])), roi: null });
    return { recipe: this.copy(recipe), nodeId };
  }
  async removeNode(recipeId: string, nodeId: string) {
    const recipe = this.need(recipeId);
    recipe.nodes = recipe.nodes.filter((n) => n.id !== nodeId);
    recipe.edges = recipe.edges.filter((e) => !e.from.startsWith(nodeId + ".") && !e.to.startsWith(nodeId + "."));
    return this.copy(recipe);
  }
  async connect(recipeId: string, from: string, to: string) {
    const recipe = this.need(recipeId);
    const a = this.typeOf(recipe, from, "outputs");
    const b = this.typeOf(recipe, to, "inputs");
    if (!a || !b || a !== b) throw new PlatformError(`端口类型不兼容：${a} → ${b}`, "authoring.type_mismatch", 422);
    recipe.edges.push({ from, to });
    return this.copy(recipe);
  }
  async disconnect(recipeId: string, from: string, to: string) {
    const recipe = this.need(recipeId);
    recipe.edges = recipe.edges.filter((e) => !(e.from === from && e.to === to));
    return this.copy(recipe);
  }
  async setParam(recipeId: string, nodeId: string, name: string, value: ParamValue) {
    const recipe = this.need(recipeId);
    const node = recipe.nodes.find((n) => n.id === nodeId);
    if (!node) throw new PlatformError(`找不到节点 ${nodeId}`, "authoring.node_not_found", 404);
    if (typeof value === "number" && value > 255) throw new PlatformError("参数 value 不能大于 255", "authoring.param_invalid", 422);
    node.params[name] = value;
    return this.copy(recipe);
  }
  async setRoi(recipeId: string, nodeId: string, roi: Roi | null) {
    const recipe = this.need(recipeId);
    const node = recipe.nodes.find((n) => n.id === nodeId);
    if (node) node.roi = roi;
    return this.copy(recipe);
  }
  async publish(recipeId: string) {
    const recipe = this.need(recipeId);
    if (recipe.nodes.length === 0) throw new PlatformError("方案不能发布：没有任何节点提供判定依据", "authoring.publish_rejected", 422);
    return `${recipeId}@v1`;
  }
  async uploadImage() {
    return { imageId: "img-1", width: 640, height: 480, channels: 1 };
  }
  async debugRun(recipeId: string): Promise<DebugRun> {
    const recipe = this.copy(this.need(recipeId));
    const number = ++this.runs;
    if (this.runDelayMs > 0) await new Promise((r) => setTimeout(r, this.runDelayMs));
    if (this.failNextRun) {
      this.failNextRun = false;
      throw new PlatformError("模拟：运行失败", "execution.failed", 422);
    }
    return {
      runId: `run-${number}`,
      verdict: "OK",
      totalMs: 1,
      reasons: [],
      result: { defects: [], measurements: [], poses: [] },
      nodes: recipe.nodes.map((n) => ({ nodeId: n.id, operatorId: n.operatorId, status: "succeeded" as const, durationMs: 0.1, message: "",
        outputs: { threshold: { type: "number", value: n.params.value } } })),
    };
  }
  imageUrl(imageId: string) {
    return `/api/images/${imageId}`;
  }
  outputUrl(relativeUrl: string) {
    return relativeUrl;
  }
}
