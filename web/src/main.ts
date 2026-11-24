import "@vue-flow/core/dist/style.css";
import "@vue-flow/core/dist/theme-default.css";
import "@vue-flow/controls/dist/style.css";
import "./style.css";

import { createApp } from "vue";
import App from "./App.vue";
import { createEditor } from "./editor";
import { HttpPlatformClient } from "./platform/HttpPlatformClient";
import { createStations } from "./stations";

// 组装：界面的其余部分只认识 editor（它后面是 PlatformClient）。换一种和后端说话的方式，只改这里的一行。
const client = new HttpPlatformClient();
const editor = createEditor(client, { storage: window.localStorage });
const stations = createStations(client);
createApp(App).provide("editor", editor).provide("stations", stations).mount("#app");
