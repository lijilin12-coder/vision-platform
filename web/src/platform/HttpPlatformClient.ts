import type { PlatformClient } from "./PlatformClient";
import { PlatformError } from "./types";
import type { Acceptance, DebugRun, GoldenSet, NewGoldenSet, ModelInfo, OperatorMeta, ParamValue, Recipe, RecipeSummary, QualitySummary, RecordQuery, Release, Roi, SavedRecord, Station, UploadedImage } from "./types";

// PlatformClient 的 HTTP 实现：对着 vision-runtime 的接口（docs/dev/http-api.md）。
// 整个界面里，只有这个文件发 HTTP 请求。
export class HttpPlatformClient implements PlatformClient {
  // base：接口的地址前缀。界面和接口由同一个服务提供时是空的
  constructor(private readonly base = "") {}

  private async request<T>(method: string, path: string, body?: unknown): Promise<T> {
    let response: Response;
    try {
      response = await fetch(this.base + path, {
        method,
        headers: body === undefined ? undefined : { "Content-Type": "application/json" },
        body: body === undefined ? undefined : JSON.stringify(body),
      });
    } catch {
      throw new PlatformError("连不上后端服务（vision-runtime 是不是没有启动？）", "client.unreachable", 0);
    }
    return this.read<T>(response);
  }

  private async read<T>(response: Response): Promise<T> {
    const data = await response.json().catch(() => null);
    if (!response.ok) {
      throw new PlatformError(data?.message ?? `后端返回了状态码 ${response.status}`, data?.code ?? "client.unknown", response.status);
    }
    return data as T;
  }

  async listOperators() {
    return (await this.request<{ operators: OperatorMeta[] }>("GET", "/api/operators")).operators;
  }
  async listModels() {
    return (await this.request<{ models: ModelInfo[] }>("GET", "/api/models")).models;
  }
  async listRecipes() {
    return (await this.request<{ recipes: RecipeSummary[] }>("GET", "/api/recipes")).recipes;
  }
  createRecipe(name: string) {
    return this.request<Recipe>("POST", "/api/recipes", { name });
  }
  getRecipe(recipeId: string) {
    return this.request<Recipe>("GET", `/api/recipes/${recipeId}`);
  }
  async addNode(recipeId: string, operatorId: string) {
    const recipe = await this.request<Recipe & { addedNodeId: string }>("POST", `/api/recipes/${recipeId}/nodes`, { operatorId });
    return { recipe, nodeId: recipe.addedNodeId };
  }
  removeNode(recipeId: string, nodeId: string) {
    return this.request<Recipe>("DELETE", `/api/recipes/${recipeId}/nodes/${nodeId}`);
  }
  connect(recipeId: string, from: string, to: string) {
    return this.request<Recipe>("POST", `/api/recipes/${recipeId}/edges`, { from, to });
  }
  disconnect(recipeId: string, from: string, to: string) {
    return this.request<Recipe>("DELETE", `/api/recipes/${recipeId}/edges`, { from, to });
  }
  setParam(recipeId: string, nodeId: string, name: string, value: ParamValue) {
    return this.request<Recipe>("PUT", `/api/recipes/${recipeId}/nodes/${nodeId}/params/${name}`, { value });
  }
  setRoi(recipeId: string, nodeId: string, roi: Roi | null) {
    const path = `/api/recipes/${recipeId}/nodes/${nodeId}/roi`;
    return roi === null ? this.request<Recipe>("DELETE", path) : this.request<Recipe>("PUT", path, roi);
  }
  async publish(recipeId: string) {
    return (await this.request<{ versionId: string }>("POST", `/api/recipes/${recipeId}/publish`)).versionId;
  }

  async uploadImage(file: Blob) {
    let response: Response;
    try {
      // Content-Type 必须是图像的类型：不写的话浏览器可能按表单发，后端对表单有 8 KB 的上限
      response = await fetch(this.base + "/api/images", {
        method: "POST",
        headers: { "Content-Type": file.type || "image/png" },
        body: file,
      });
    } catch {
      throw new PlatformError("连不上后端服务（vision-runtime 是不是没有启动？）", "client.unreachable", 0);
    }
    return this.read<UploadedImage>(response);
  }
  debugRun(recipeId: string, imageId: string) {
    return this.request<DebugRun>("POST", `/api/recipes/${recipeId}/debug-runs`, { imageId });
  }

  async listReleases() {
    return (await this.request<{ releases: Release[] }>("GET", "/api/releases")).releases;
  }
  async createRelease(recipeVersionId: string) {
    return (await this.request<{ releaseId: string }>("POST", "/api/releases", { recipeVersionId })).releaseId;
  }
  async listGoldenSets() {
    return (await this.request<{ goldenSets: GoldenSet[] }>("GET", "/api/golden-sets")).goldenSets;
  }
  createGoldenSet(definition: NewGoldenSet) {
    return this.request<GoldenSet>("POST", "/api/golden-sets", definition);
  }
  checkAcceptance(releaseId: string, goldenSetId: string) {
    return this.request<Acceptance>("POST", `/api/releases/${releaseId}/acceptance`, { goldenSetId });
  }
  async exportRelease(releaseId: string, path: string) {
    await this.request("POST", `/api/releases/${releaseId}/export`, { path });
  }
  async listStations() {
    return (await this.request<{ stations: Station[] }>("GET", "/api/stations")).stations;
  }
  createStation(name: string) {
    return this.request<Station>("POST", "/api/stations", { name });
  }
  getStation(stationId: string, afterSequence = 0) {
    return this.request<Station>("GET", `/api/stations/${stationId}?after=${afterSequence}`);
  }
  bindImageFolder(stationId: string, directory: string) {
    return this.request<Station>("PUT", `/api/stations/${stationId}/image-source`, { kind: "folder", location: directory });
  }
  deploy(stationId: string, releaseId: string) {
    return this.request<Station>("POST", `/api/stations/${stationId}/deploy`, { releaseId });
  }
  rollback(stationId: string) {
    return this.request<Station>("POST", `/api/stations/${stationId}/rollback`, {});
  }
  startStation(stationId: string) {
    return this.request<Station>("POST", `/api/stations/${stationId}/start`, {});
  }
  stopStation(stationId: string) {
    return this.request<Station>("POST", `/api/stations/${stationId}/stop`, {});
  }
  async listRecords(stationId: string, query: RecordQuery = {}) {
    const params = new URLSearchParams();
    if (query.verdict) params.set("verdict", query.verdict);
    if (query.before) params.set("before", query.before);
    if (query.limit) params.set("limit", String(query.limit));
    return (await this.request<{ records: SavedRecord[] }>("GET", `/api/stations/${stationId}/records?${params}`)).records;
  }
  getSummary(stationId: string) {
    return this.request<QualitySummary>("GET", `/api/stations/${stationId}/summary`);
  }
  stationFrameUrl(stationId: string, sequence: number) {
    return `${this.base}/api/stations/${stationId}/frame?s=${sequence}`;
  }

  imageUrl(imageId: string) {
    return `${this.base}/api/images/${imageId}`;
  }
  outputUrl(relativeUrl: string) {
    return this.base + relativeUrl;
  }
}
