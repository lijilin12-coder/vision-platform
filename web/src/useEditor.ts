import { inject } from "vue";
import type { Editor } from "./editor";

export function useEditor(): Editor {
  const editor = inject<Editor>("editor");
  if (!editor) throw new Error("没有提供 editor");
  return editor;
}

// 端口类型 → 颜色。连线的两头颜色一样才连得上，一眼能看出来
export const portColors: Record<string, string> = {
  image: "#3b82f6",
  region: "#10b981",
  blobs: "#f59e0b",
  number: "#8b5cf6",
  bool: "#ec4899",
  text: "#6b7280",
  judgement: "#ef4444",
  pose: "#14b8a6",
};

export const portTypeNames: Record<string, string> = {
  image: "图像",
  region: "区域",
  blobs: "连通块",
  number: "数值",
  bool: "布尔",
  text: "文本",
  judgement: "判定依据",
  pose: "位姿",
};
