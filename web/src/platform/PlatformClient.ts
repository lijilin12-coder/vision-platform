import type { DebugRun, ModelInfo, OperatorMeta, ParamValue, Recipe, RecipeSummary, Release, Roi, Station, UploadedImage } from "./types";

// 界面使用平台的唯一入口。
//
// 界面的组件、状态只认识这个接口，不知道后面是 HTTP 还是别的。现在只有一个实现（HttpPlatformClient）；
// 以后在鸿蒙上，界面跑在 ArkWeb 里，会再写一个通过桥调用核心的实现——界面的其余部分不用改。
// 所以：除了 HttpPlatformClient.ts，别的文件里都不应该出现 fetch(。
//
// 约定：操作被后端拒绝时抛出 PlatformError，它的 message 就是后端给的原因，可以直接显示给人看。
export interface PlatformClient {
  listOperators(): Promise<OperatorMeta[]>;
  listModels(): Promise<ModelInfo[]>;

  listRecipes(): Promise<RecipeSummary[]>;
  createRecipe(name: string): Promise<Recipe>;
  getRecipe(recipeId: string): Promise<Recipe>;
  addNode(recipeId: string, operatorId: string): Promise<{ recipe: Recipe; nodeId: string }>;
  removeNode(recipeId: string, nodeId: string): Promise<Recipe>;
  connect(recipeId: string, from: string, to: string): Promise<Recipe>;
  disconnect(recipeId: string, from: string, to: string): Promise<Recipe>;
  setParam(recipeId: string, nodeId: string, name: string, value: ParamValue): Promise<Recipe>;
  setRoi(recipeId: string, nodeId: string, roi: Roi | null): Promise<Recipe>;
  publish(recipeId: string): Promise<string>; // 返回方案版本 ID

  uploadImage(file: Blob): Promise<UploadedImage>;
  debugRun(recipeId: string, imageId: string): Promise<DebugRun>;

  // ---- 上线包和工位 ----
  listReleases(): Promise<Release[]>;
  createRelease(recipeVersionId: string): Promise<string>; // 返回上线包 ID
  listStations(): Promise<Station[]>;
  createStation(name: string): Promise<Station>;
  // afterSequence：只要序号比它大的检测记录
  getStation(stationId: string, afterSequence?: number): Promise<Station>;
  bindImageFolder(stationId: string, directory: string): Promise<Station>;
  deploy(stationId: string, releaseId: string): Promise<Station>;
  rollback(stationId: string): Promise<Station>;
  startStation(stationId: string): Promise<Station>;
  stopStation(stationId: string): Promise<Station>;
  // 工位最近检测的那一帧。sequence 只是为了让地址每次不同（浏览器不要拿旧图充数）
  stationFrameUrl(stationId: string, sequence: number): string;

  // 返回可以直接放进 <img src> 的地址。（不返回字节：鸿蒙的桥不一定传得了二进制，那时可以换成别的地址形式。）
  imageUrl(imageId: string): string;
  outputUrl(relativeUrl: string): string;
}
