import vue from "@vitejs/plugin-vue";
import { defineConfig } from "vite";

// 开发时（npm run dev）：界面由 Vite 提供，/api 开头的请求转给本机的 vision-runtime。
// 发布时（npm run build）：生成 dist 目录，由 vision-runtime 自己提供（见 apps/runtime）。
export default defineConfig({
  plugins: [vue()],
  server: {
    port: 5173,
    proxy: { "/api": "http://127.0.0.1:8750" },
  },
  build: { outDir: "dist", emptyOutDir: true, chunkSizeWarningLimit: 900 },
});
