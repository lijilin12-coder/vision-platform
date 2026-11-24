import "@vue-flow/core/dist/style.css";
import "@vue-flow/core/dist/theme-default.css";
import "@vue-flow/controls/dist/style.css";
import "./style.css";

import { createApp } from "vue";
import App from "./App.vue";
import { createEditor } from "./editor";
import { HttpPlatformClient } from "./platform/HttpPlatformClient";

// 组装：界面的其余部分只认识 editor（它后面是 PlatformClient）。换一种和后端说话的方式，只改这里的一行。
const editor = createEditor(new HttpPlatformClient(), { storage: window.localStorage });
createApp(App).provide("editor", editor).mount("#app");
