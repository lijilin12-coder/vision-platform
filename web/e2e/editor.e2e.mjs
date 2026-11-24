// 端到端测试：真的浏览器 + 真的 vision-runtime + 真的界面文件（web/dist）。
//
//   先编译：  cmake --build build（仓库根目录）；npm run build（web 目录）
//   再运行：  npm run e2e
//
// 检查的是 POC-2 的通过标准（docs/poc/poc-2-plan.md §7）：
//   1 从空白搭出“灰度 → 阈值 → Blob → 判定”，上传图像，运行，得到判定
//   2 点一个节点，看到它输出的图像
//   3 改一个参数到预览更新，不超过 300 毫秒
//   4 不合规则的操作被拒绝，界面显示后端给的原因
//   5 界面上搭的方案，命令行能看到
//
// 环境变量 SCREENSHOT=<目录> 时，把几个关键时刻的画面存成图片。

import { execFileSync, spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, readdirSync, rmSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const { chromium } = require("playwright");

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const exe = (name) => join(root, "build", "apps", process.platform === "win32" ? `${name}.exe` : name);
const port = 8790 + Math.floor(Math.random() * 100);
const base = `http://127.0.0.1:${port}`;
const work = mkdtempSync(join(tmpdir(), "vision-e2e-"));
const data = join(work, "数据"); // 中文数据目录
const images = join(work, "images");
const shots = process.env.SCREENSHOT;

let failures = 0;
function check(condition, what) {
  console.log(`${condition ? "  ✓" : "  ✗"} ${what}`);
  if (!condition) failures++;
}

// 示例图像：2 幅合格的、2 幅带亮斑的
execFileSync(exe("vision-cli"), ["--data", data, "sample-images", images]);

const server = spawn(exe("vision-runtime"), ["--data", data, "--port", String(port), "--web", join(root, "web", "dist")], { stdio: "ignore" });
async function waitForServer() {
  for (let i = 0; i < 100; i++) {
    try {
      if ((await fetch(`${base}/api/health`)).ok) return;
    } catch {
      /* 还没起来 */
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error("vision-runtime 没有启动起来");
}

const browser = await chromium.launch();
try {
  await waitForServer();
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: shots ? 2 : 1 });
  const consoleErrors = [];
  // 后端拒绝一个操作（422）时浏览器会自动在控制台记一笔，那是预期之中的；别的报错才算问题
  page.on("console", (m) => m.type() === "error" && !/status of 422/.test(m.text()) && consoleErrors.push(m.text()));
  page.on("pageerror", (e) => consoleErrors.push(String(e)));
  const shot = async (name) => {
    if (!shots) return;
    mkdirSync(shots, { recursive: true });
    await page.screenshot({ path: join(shots, `${name}.png`) });
  };

  await page.goto(base);
  await page.waitForSelector('[data-operator="vision.threshold"]');
  check((await page.locator("[data-operator]").count()) >= 18, "算子面板列出了后端的全部算子");

  console.log("标准 1：从空白搭出方案");
  await page.fill("[data-new-name]", "界面搭的亮斑检测");
  await page.click("[data-new]");
  for (const op of ["image_input", "to_gray", "threshold", "blob", "judge_count"]) {
    const before = await page.locator("[data-node]").count();
    await page.click(`[data-operator="vision.${op}"]`);
    await page.waitForFunction((n) => document.querySelectorAll("[data-node]").length === n + 1, before);
  }
  check((await page.locator("[data-node]").count()) === 5, "5 个节点都出现在画布上");

  // 连线：在画布上从一个端口拖到另一个端口
  const handle = (node, name, type) => page.locator(`.vue-flow__handle.${type}[data-nodeid="${node}"][data-handleid="${name}"]`);
  await page.waitForTimeout(300); // 等视野调整完
  async function connect(from, fromPort, to, toPort) {
    const a = await handle(from, fromPort, "source").boundingBox();
    const b = await handle(to, toPort, "target").boundingBox();
    const edges = await page.locator(".vue-flow__edge").count();
    await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2);
    await page.mouse.down();
    await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2, { steps: 10 });
    await page.mouse.up();
    await page.waitForFunction((n) => document.querySelectorAll(".vue-flow__edge").length === n + 1, edges, { timeout: 3000 }).catch(() => {});
    return (await page.locator(".vue-flow__edge").count()) === edges + 1;
  }
  check(await connect("n1", "image", "n2", "image"), "连线 输入 → 灰度");
  check(await connect("n2", "image", "n3", "image"), "连线 灰度 → 阈值");
  check(await connect("n3", "region", "n4", "region"), "连线 阈值 → Blob");
  check(await connect("n4", "blobs", "n5", "blobs"), "连线 Blob → 判定");

  console.log("标准 4（界面的提示）：类型对不上的端口，拖过去连不上");
  check(!(await connect("n1", "image", "n4", "region")), "图像 → 区域 没有连上");
  check((await page.locator(".vue-flow__edge").count()) === 4, "连线还是 4 条");

  // 改参数：阈值 200
  await page.click('[data-node="n3"]');
  const valueInput = page.locator('[data-param="value"] input');
  await valueInput.fill("200");
  await valueInput.press("Tab");
  await page.waitForFunction(() => document.querySelector('[data-param="value"] input')?.value === "200");

  // 上传一幅有 3 个亮斑的图像 → 自动运行
  await page.setInputFiles("[data-file]", join(images, "ng_02_three_spots.png"));
  await page.waitForSelector("[data-verdict]");
  check((await page.textContent("[data-verdict]")) === "NG", "有 3 个亮斑的图像：判定 NG");
  check((await page.locator("svg .defect").count()) === 3, "3 个缺陷用框标在了图像上");
  check((await page.locator(".result").textContent()).includes("3 处"), "判定依据显示出来了");
  await shot("1-ng");

  console.log("标准 2：点一个节点，看到它输出的图像");
  await page.click('[data-node="n3"]');
  await page.waitForFunction(() => document.querySelector("[data-preview]")?.getAttribute("src")?.includes("/outputs/n3/region"));
  const loaded = await page.evaluate(() => {
    const img = document.querySelector("[data-preview]");
    return img.decode().then(() => ({ w: img.naturalWidth, h: img.naturalHeight }));
  });
  check(loaded.w === 640 && loaded.h === 480, `阈值节点的区域图显示出来了（${loaded.w}×${loaded.h}）`);
  await shot("2-node-output");

  console.log("标准 3：改一个参数到预览更新");
  const timings = [];
  for (const value of ["180", "190", "200"]) {
    const srcBefore = await page.getAttribute("[data-preview]", "src");
    const t0 = Date.now();
    await valueInput.fill(value);
    await valueInput.press("Tab");
    await page.waitForFunction((s) => document.querySelector("[data-preview]")?.getAttribute("src") !== s, srcBefore);
    await page.evaluate(() => document.querySelector("[data-preview]").decode());
    timings.push(Date.now() - t0);
  }
  const best = Math.min(...timings);
  console.log(`    三次分别用了 ${timings.join("、")} 毫秒（其中 200 毫秒是故意等的：等人停止输入）`);
  check(best - 200 < 300, `扣除故意等的 200 毫秒，最快一次 ${best - 200} 毫秒，不超过 300`);

  console.log("换一幅合格的图像");
  await page.setInputFiles("[data-file]", join(images, "ok_01.png"));
  await page.waitForFunction(() => document.querySelector("[data-verdict]")?.textContent === "OK");
  check((await page.locator("svg .defect").count()) === 0, "合格的图像：判定 OK，没有缺陷框");

  console.log("标准 4：不合规则的操作被拒绝，显示后端给的原因");
  await valueInput.fill("999");
  await valueInput.press("Tab");
  await page.waitForSelector("[data-error]");
  check((await page.textContent("[data-error]")).includes("不能大于 255"), "超出范围的参数：显示了后端的原因");
  await shot("3-rejected");
  await page.click("[data-error]");

  console.log("发布");
  await page.click("[data-publish]");
  await page.waitForSelector("[data-notice]");
  const notice = await page.textContent("[data-notice]");
  check(/@v1/.test(notice), `发布成功：${notice.trim().split(" ")[0]}`);

  console.log("标准 5：界面上搭的方案，命令行能看到；刷新之后还在");
  const recipeId = await page.inputValue("[data-recipe-select]");
  const shown = execFileSync(exe("vision-cli"), ["--data", data, "recipe", "show", recipeId]).toString();
  check(shown.includes("界面搭的亮斑检测") && shown.includes("vision.judge_count"), "vision-cli recipe show 能看到它和它的节点");
  await page.reload();
  await page.waitForSelector("[data-recipe-select] option:nth-child(2)", { state: "attached" });
  await page.selectOption("[data-recipe-select]", { label: "界面搭的亮斑检测" });
  await page.waitForFunction(() => document.querySelectorAll("[data-node]").length === 5);
  check((await page.locator(".vue-flow__edge").count()) === 4, "刷新后重新打开：5 个节点、4 条连线都在");

  check(consoleErrors.length === 0, `浏览器控制台没有报错${consoleErrors.length ? "：" + consoleErrors.slice(0, 3).join(" | ") : ""}`);
} finally {
  await browser.close();
  server.kill();
  rmSync(work, { recursive: true, force: true });
}

console.log(failures === 0 ? "\n全部通过" : `\n有 ${failures} 项没有通过`);
process.exit(failures === 0 ? 0 : 1);
