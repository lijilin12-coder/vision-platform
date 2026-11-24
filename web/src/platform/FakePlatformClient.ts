import type { PlatformClient } from "./PlatformClient";
import { PlatformError } from "./types";
import type { DebugRun, InspectionRecord, OperatorMeta, ParamValue, Recipe, Release, Roi, Station, QualitySummary, RecordQuery, SavedRecord } from "./types";

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
  // ---- 上线包和工位：同样只有测试需要的那一点 ----
  releases: Release[] = [];
  stations = new Map<string, Station>();
  produced: InspectionRecord[] = []; // 测试往这里放“工位新检测出来的记录”，下一次查询时交出去

  private station(id: string): Station {
    const s = this.stations.get(id);
    if (!s) throw new PlatformError(`找不到工位 ${id}`, "station.not_found", 404);
    return s;
  }
  async listReleases() {
    return this.releases;
  }
  async createRelease(recipeVersionId: string) {
    const id = `rel-${this.releases.length + 1}`;
    this.releases.push({ id, recipeVersionId, recipeName: recipeVersionId, withdrawn: false });
    return id;
  }
  async listStations() {
    return [...this.stations.values()].map((s) => ({ ...s, recent: [] }));
  }
  async createStation(name: string) {
    const s: Station = { id: `s${this.stations.size + 1}`, name, state: "stopped", activeRelease: "", pendingRelease: "", pendingReady: false,
      releaseHistory: [], consecutiveErrors: 0, sequence: 0, imageSource: { kind: "", location: "" },
      runner: { running: false, idle: false, ok: 0, ng: 0, error: 0, unreadable: 0, stoppedReason: "" }, recent: [] };
    this.stations.set(s.id, s);
    return { ...s };
  }
  async getStation(stationId: string, afterSequence = 0) {
    const s = this.station(stationId);
    for (const r of this.produced.splice(0)) {
      s.sequence = r.sequence;
      s.runner[r.verdict === "OK" ? "ok" : r.verdict === "NG" ? "ng" : "error"]++;
      s.recent.push(r);
    }
    return { ...s, runner: { ...s.runner }, recent: s.recent.filter((r) => r.sequence > afterSequence) };
  }
  async bindImageFolder(stationId: string, directory: string) {
    const s = this.station(stationId);
    if (s.runner.running) throw new PlatformError("工位停止之后才能更换图像源", "station.source_while_running", 422);
    s.imageSource = { kind: "folder", location: directory };
    return { ...s };
  }
  async deploy(stationId: string, releaseId: string) {
    const s = this.station(stationId);
    if (!this.releases.some((r) => r.id === releaseId)) throw new PlatformError(`找不到上线包 ${releaseId}`, "release.not_found", 404);
    if (s.runner.running) Object.assign(s, { pendingRelease: releaseId, pendingReady: true });
    else s.activeRelease = releaseId;
    return { ...s };
  }
  async rollback(stationId: string) {
    return { ...this.station(stationId) };
  }
  async startStation(stationId: string) {
    const s = this.station(stationId);
    if (!s.activeRelease) throw new PlatformError("工位还没有可用的上线包，请先部署", "station.no_active_release", 422);
    s.state = "running";
    s.runner.running = true;
    return { ...s, runner: { ...s.runner } };
  }
  async stopStation(stationId: string) {
    const s = this.station(stationId);
    s.state = "stopped";
    s.runner.running = false;
    return { ...s, runner: { ...s.runner } };
  }
  saved: SavedRecord[] = []; // 测试往这里放“保存下来的检测记录”，从旧到新
  async listRecords(stationId: string, query: RecordQuery = {}) {
    this.station(stationId);
    return [...this.saved]
      .reverse()
      .filter((r) => (!query.verdict || r.verdict === query.verdict) && (!query.before || r.id < query.before))
      .slice(0, query.limit ?? 50);
  }
  async getSummary(stationId: string): Promise<QualitySummary> {
    this.station(stationId);
    const count = (v: string) => this.saved.filter((r) => r.verdict === v).length;
    const [ok, ng] = [count("OK"), count("NG")];
    const defectsByCategory: Record<string, number> = {};
    for (const r of this.saved) for (const d of r.result.defects) defectsByCategory[d.category] = (defectsByCategory[d.category] ?? 0) + 1;
    return { total: this.saved.length, ok, ng, error: count("ERROR"), yield: ok + ng > 0 ? ok / (ok + ng) : null, defectsByCategory };
  }
  stationFrameUrl(stationId: string, sequence: number) {
    return `/api/stations/${stationId}/frame?s=${sequence}`;
  }

  imageUrl(imageId: string) {
    return `/api/images/${imageId}`;
  }
  outputUrl(relativeUrl: string) {
    return relativeUrl;
  }
}
