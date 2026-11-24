// 界面和后端之间交换的数据的形状。和 docs/dev/http-api.md 一一对应。

export type ParamValue = number | boolean | string;

export interface PortDef {
  name: string;
  type: string; // image / region / blobs / number / bool / text / judgement / pose
  optional: boolean;
}

export interface ParamDef {
  name: string;
  type: "number" | "bool" | "text" | "choice" | "template" | "model";
  default: ParamValue;
  label: string;
  min?: number;
  max?: number;
  choices?: string[];
}

export interface OperatorMeta {
  id: string;
  majorVersion: number;
  displayName: string;
  category: string;
  description: string;
  inputs: PortDef[];
  outputs: PortDef[];
  params: ParamDef[];
  contributesToVerdict: boolean;
}

export interface Roi {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface RecipeNode {
  id: string;
  operatorId: string;
  params: Record<string, ParamValue>;
  roi: Roi | null;
}

export interface RecipeEdge {
  from: string; // "n1.image"
  to: string;
}

export interface RecipeTemplate {
  id: string;
  name: string;
  width: number;
  height: number;
}

export interface Recipe {
  id: string;
  name: string;
  nodes: RecipeNode[];
  edges: RecipeEdge[];
  templates: RecipeTemplate[];
}

export interface RecipeSummary {
  id: string;
  name: string;
}

export interface UploadedImage {
  imageId: string;
  width: number;
  height: number;
  channels: number;
}

export interface OutputValue {
  type: string;
  summary?: string;
  statistics?: string;
  hasImage?: boolean;
  imageUrl?: string;
  width?: number;
  height?: number;
  value?: unknown;
}

export interface NodeRun {
  nodeId: string;
  operatorId: string;
  status: "succeeded" | "failed" | "skipped";
  durationMs: number;
  message: string;
  outputs: Record<string, OutputValue>;
}

export interface Defect {
  nodeId: string;
  category: string;
  centerX: number;
  centerY: number;
  left: number;
  top: number;
  width: number;
  height: number;
  area: number;
}

export interface Measurement {
  nodeId: string;
  name: string;
  value: number;
  unit: string;
  pass: boolean;
  lower?: number;
  upper?: number;
}

export interface PoseResult {
  nodeId: string;
  found: boolean;
  score: number;
  x: number;
  y: number;
  angle: number;
}

export interface DebugRun {
  runId: string;
  verdict: "OK" | "NG" | "ERROR";
  totalMs: number;
  reasons: { nodeId: string; pass: boolean; text: string }[];
  result: { defects: Defect[]; measurements: Measurement[]; poses: PoseResult[] };
  nodes: NodeRun[];
}

export interface ModelInfo {
  id: string;
  status: string;
  kind: string;
}

export interface Release {
  id: string;
  recipeVersionId: string;
  recipeName: string;
  withdrawn: boolean;
}

// 工位上的一次检测
export interface InspectionRecord {
  sequence: number;
  frameName: string;
  verdict: "OK" | "NG" | "ERROR";
  releaseId: string;
  swapped: boolean; // 这次检测开始前发生了热切换
  totalMs: number;
  reasons: { nodeId: string; pass: boolean; text: string }[];
  result: { defects: Defect[]; measurements: Measurement[]; poses: PoseResult[] };
}

// 保存下来的检测记录：比上面多了编号、时间、当时的图像。程序重启之后还在
export interface SavedRecord extends InspectionRecord {
  id: string; // 记录的编号：越新的越大
  time: string; // 给人看的时间（UTC）
  recipeVersionId: string;
  imageId: string; // 当时的图像；没有保留时是空的（默认只保留不合格的和出错的）
}

// 一个工位有记录以来的汇总
export interface QualitySummary {
  total: number;
  ok: number;
  ng: number;
  error: number;
  yield: number | null; // 良率：OK ÷ (OK + NG)。还没有可算的时候是 null
  defectsByCategory: Record<string, number>;
}

export interface RecordQuery {
  verdict?: "OK" | "NG" | "ERROR";
  before?: string; // 翻页：只要比这个编号更早的
  limit?: number;
}

export interface Station {
  id: string;
  name: string;
  state: "stopped" | "running" | "paused" | "faulted";
  activeRelease: string;
  pendingRelease: string;
  pendingReady: boolean;
  releaseHistory: string[];
  consecutiveErrors: number;
  sequence: number;
  imageSource: { kind: string; location: string };
  runner: { running: boolean; idle: boolean; ok: number; ng: number; error: number; unreadable: number; stoppedReason: string };
  recent: InspectionRecord[];
}

// 后端拒绝了一个操作（规则不允许、找不到……）。message 是给人看的，原样来自后端
export class PlatformError extends Error {
  constructor(
    message: string,
    readonly code: string,
    readonly status: number,
  ) {
    super(message);
  }
}
