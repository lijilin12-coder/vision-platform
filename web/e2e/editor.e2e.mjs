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
import { copyFileSync, mkdtempSync, mkdirSync, rmSync } from "node:fs";
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

  console.log("用鼠标画 ROI");
  await page.setInputFiles("[data-file]", join(images, "ng_02_three_spots.png")); // 3 个亮斑，分别在 (170,160) (260,205) (350,250)
  await page.waitForFunction(() => document.querySelector("[data-verdict]")?.textContent === "NG");
  await page.click('[data-node="n3"]');
  await page.click("[data-draw-roi]");
  const frame = await page.locator("[data-overlay]").boundingBox();
  const at = (x, y) => [frame.x + (x / 640) * frame.width, frame.y + (y / 480) * frame.height];
  // 只框住左上的那一个亮斑：图像上 (120,110) 到 (220,190)
  await page.mouse.move(...at(120, 110));
  await page.mouse.down();
  await page.mouse.move(...at(180, 150), { steps: 5 });
  await page.mouse.move(...at(220, 190), { steps: 5 });
  await page.mouse.up();
  await page.waitForSelector("[data-roi]");
  const roi = await page.evaluate(() => {
    const r = document.querySelector("[data-roi]");
    return ["x", "y", "width", "height"].map((k) => Number(r.getAttribute(k)));
  });
  check(Math.abs(roi[0] - 120) <= 3 && Math.abs(roi[1] - 110) <= 3 && Math.abs(roi[2] - 100) <= 4 && Math.abs(roi[3] - 80) <= 4,
        `画出来的 ROI 是图像上的像素坐标：x ${roi[0]} y ${roi[1]} 宽 ${roi[2]} 高 ${roi[3]}`);
  await page.waitForFunction(() => document.querySelectorAll("svg .defect").length === 1);
  check((await page.locator(".result").textContent()).includes("1 处"), "只在 ROI 里找：缺陷从 3 处变成 1 处");
  await shot("4-roi");
  await page.click("[data-clear-roi]");
  await page.waitForFunction(() => document.querySelectorAll("svg .defect").length === 3);
  check((await page.locator("[data-roi]").count()) === 0, "清除 ROI：又是 3 处");

  console.log("工位页面：发布 → 做成上线包 → 部署 → 开始 → 运行中换上线包");
  await page.click("[data-publish]");
  await page.waitForSelector("[data-make-release]");
  await page.click("[data-make-release]");
  await page.waitForFunction(() => document.querySelector("[data-notice]")?.textContent.includes("已做成上线包"));
  // 再做一个宽松的版本（最多允许 5 个亮斑），也做成上线包
  await page.click('[data-node="n5"]');
  const maxCount = page.locator('[data-param="maxCount"] input');
  await maxCount.fill("5");
  await maxCount.press("Tab");
  await page.waitForFunction(() => document.querySelector("[data-verdict]")?.textContent === "OK");
  await page.click("[data-publish]");
  await page.waitForSelector("[data-make-release]");
  await page.click("[data-make-release]");
  await page.waitForFunction(() => !document.querySelector("[data-make-release]"));

  await page.click('[data-tab="stations"]');
  await page.fill("[data-station-name]", "一号工位");
  await page.click("[data-station-new]");
  await page.waitForSelector("[data-station-state]");
  check((await page.locator("[data-station-release] option").count()) === 3, "两个上线包都在可以部署的列表里");
  // 还没部署就点开始：后端拒绝，原因显示出来
  const lineImages = join(work, "来料");
  mkdirSync(lineImages);
  copyFileSync(join(images, "ng_02_three_spots.png"), join(lineImages, "1.png"));
  await page.fill("[data-station-folder]", lineImages);
  await page.click("[data-station-bind]");
  await page.click("[data-station-start]");
  await page.waitForSelector("[data-station-error]");
  check((await page.textContent("[data-station-error]")).includes("上线包"), "没有部署就开始：显示了后端的原因");
  await page.click("[data-station-error]");
  // 部署严格的那个，开始。列表的顺序不固定，按方案的版本号选：
  // 前面“发布”那一步发过 v1（没有做成上线包），所以严格的是 v2，宽松的是 v3
  const optionFor = async (version) => {
    const labels = await page.locator("[data-station-release] option").allTextContents();
    const found = labels.find((l) => l.includes(` ${version}（`));
    if (!found) throw new Error(`下拉框里没有 ${version} 的上线包：${JSON.stringify(labels)}`);
    return found;
  };
  await page.selectOption("[data-station-release]", { label: await optionFor("v2") });
  await page.click("[data-station-deploy]");
  await page.waitForFunction(() => !document.querySelector("[data-station-active]")?.textContent.includes("无") || document.body.textContent.includes("已就绪"));
  await page.click("[data-station-start]");
  await page.waitForFunction(() => document.querySelector("[data-count-ng]")?.textContent.trim() === "NG 1");
  check((await page.textContent("[data-station-state]")) === "运行中", "工位在运行，第一幅图像判 NG");
  await page.waitForFunction(() => document.querySelector("[data-station-frame]")?.complete && document.querySelector("[data-station-frame]").naturalWidth === 640);
  check((await page.locator(".overlay .defect").count()) === 3, "最近一帧显示出来了，上面有 3 个缺陷框");
  // 运行中部署宽松的那个；放进一幅新图像 → 检测开始前切换 → 同样的图像这次是 OK
  await page.selectOption("[data-station-release]", { label: await optionFor("v3") });
  await page.click("[data-station-deploy]");
  await page.waitForFunction(() => document.body.textContent.includes("已就绪"));
  copyFileSync(join(images, "ng_02_three_spots.png"), join(lineImages, "2.png"));
  await page.waitForFunction(() => document.querySelector("[data-count-ok]")?.textContent.trim() === "OK 1");
  check((await page.locator("[data-swapped]").count()) === 1, "运行中换了上线包：记录里标出了从哪一次开始切换");
  check((await page.locator("[data-record]").count()) === 2, "两次检测都在记录里");
  await shot("5-station");
  await page.click("[data-station-rollback]");
  copyFileSync(join(images, "ng_02_three_spots.png"), join(lineImages, "3.png"));
  await page.waitForFunction(() => document.querySelector("[data-count-ng]")?.textContent.trim() === "NG 2");
  check(true, "回滚之后，同样的图像又判 NG");
  await page.click("[data-station-stop]");
  await page.waitForFunction(() => document.querySelector("[data-station-state]")?.textContent === "已停止");
  check(true, "停止");

  check(consoleErrors.length === 0, `浏览器控制台没有报错${consoleErrors.length ? "：" + consoleErrors.slice(0, 3).join(" | ") : ""}`);
} finally {
  await browser.close();
  server.kill();
  rmSync(work, { recursive: true, force: true });
}

console.log(failures === 0 ? "\n全部通过" : `\n有 ${failures} 项没有通过`);
process.exit(failures === 0 ? 0 : 1);
