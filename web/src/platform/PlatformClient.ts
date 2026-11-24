import type { DebugRun, ModelInfo, OperatorMeta, ParamValue, Recipe, RecipeSummary, Roi, UploadedImage } from "./types";

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

  // 返回可以直接放进 <img src> 的地址。（不返回字节：鸿蒙的桥不一定传得了二进制，那时可以换成别的地址形式。）
  imageUrl(imageId: string): string;
  outputUrl(relativeUrl: string): string;
}
