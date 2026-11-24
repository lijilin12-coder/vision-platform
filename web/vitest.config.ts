import { defineConfig } from "vitest/config";

// 单元测试的配置。和 vite.config.ts 分开：测试只测编辑器的状态逻辑（纯 TypeScript），不需要 Vue 的插件。
export default defineConfig({
  test: { environment: "node", include: ["src/**/*.test.ts"] },
});
