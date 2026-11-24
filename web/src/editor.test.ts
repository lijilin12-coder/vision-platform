import { describe, expect, it } from "vitest";
import { createEditor } from "./editor";
import { FakePlatformClient } from "./platform/FakePlatformClient";

// 编辑器的状态和操作，对着假的后端测。不需要 vision-runtime，也不需要浏览器。

async function setup() {
  const client = new FakePlatformClient();
  const saved = new Map<string, string>();
  const storage = { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v) };
  const editor = createEditor(client, { autoRunDelayMs: 5, storage });
  await editor.init();
  await editor.newRecipe("测试");
  return { client, editor, saved, storage };
}

describe("编辑器", () => {
  it("启动时取到算子；新建方案后它出现在列表里", async () => {
    const { editor } = await setup();
    expect(editor.state.operators.length).toBe(3);
    expect(editor.state.recipe?.name).toBe("测试");
    expect(editor.state.recipes.map((r) => r.name)).toEqual(["测试"]);
  });

  it("加节点、连线：每一步都采用后端回复的方案", async () => {
    const { editor } = await setup();
    await editor.addNode("vision.image_input");
    await editor.addNode("vision.threshold");
    expect(editor.state.selectedNodeId).toBe("n2"); // 新加的节点被选中
    expect(await editor.connect("n1.image", "n2.image")).toBe(true);
    expect(editor.state.recipe?.edges).toEqual([{ from: "n1.image", to: "n2.image" }]);
    expect(editor.state.recipe?.nodes[1].params.value).toBe(128); // 参数的默认值是后端给的，不是界面猜的
  });

  it("后端拒绝的操作：本地的方案不变，原因显示出来", async () => {
    const { editor } = await setup();
    await editor.addNode("vision.image_input");
    await editor.addNode("vision.blob");
    expect(editor.canConnect("n1.image", "n2.region")).toBe(false); // 界面先提示一下
    expect(await editor.connect("n1.image", "n2.region")).toBe(false); // 硬要连：后端拒绝
    expect(editor.state.error).toContain("端口类型不兼容");
    expect(editor.state.recipe?.edges).toEqual([]);

    await editor.addNode("vision.threshold");
    expect(await editor.setParam("n3", "value", 999)).toBe(false);
    expect(editor.state.error).toContain("不能大于 255");
    expect(editor.state.recipe?.nodes[2].params.value).toBe(128);
    // 下一个成功的操作会清掉出错信息
    expect(await editor.setParam("n3", "value", 200)).toBe(true);
    expect(editor.state.error).toBeNull();
  });

  it("选了图像之后，改参数会自动重新运行；连着改几次只运行一次", async () => {
    const { client, editor } = await setup();
    await editor.addNode("vision.threshold");
    expect(client.runs).toBe(0); // 还没有图像：不运行
    await editor.uploadImage(new Blob());
    expect(client.runs).toBe(1); // 选图之后立即运行一次
    void editor.setParam("n1", "value", 100);
    void editor.setParam("n1", "value", 110);
    await editor.setParam("n1", "value", 120);
    await editor.settled();
    expect(client.runs).toBe(2);
    expect(editor.state.run?.nodes[0].outputs.threshold.value).toBe(120);
    expect(editor.state.lastRunMs).not.toBeNull();
  });

  it("过时的运行结果被丢掉：只留最后一次改动对应的结果", async () => {
    const { client, editor } = await setup();
    await editor.addNode("vision.threshold");
    await editor.uploadImage(new Blob());
    client.runDelayMs = 40; // 运行变慢：第一次还没回来，又改了一次
    await editor.setParam("n1", "value", 50);
    await new Promise((r) => setTimeout(r, 15)); // 第一次运行已经发出去了
    client.runDelayMs = 0;
    await editor.setParam("n1", "value", 60);
    await editor.settled();
    await new Promise((r) => setTimeout(r, 60)); // 等慢的那一次回来
    expect(editor.state.run?.nodes[0].outputs.threshold.value).toBe(60);
    expect(editor.state.running).toBe(false);
  });

  it("运行失败：结果清空，原因显示出来，之后还能再运行", async () => {
    const { client, editor } = await setup();
    await editor.addNode("vision.threshold");
    await editor.uploadImage(new Blob());
    client.failNextRun = true;
    await editor.run();
    expect(editor.state.run).toBeNull();
    expect(editor.state.error).toContain("运行失败");
    await editor.run();
    expect(editor.state.run?.verdict).toBe("OK");
  });

  it("删节点：它的连线一起消失；选中的是它的话取消选中", async () => {
    const { editor } = await setup();
    await editor.addNode("vision.image_input");
    await editor.addNode("vision.threshold");
    await editor.connect("n1.image", "n2.image");
    editor.select("n2");
    await editor.removeNode("n2");
    expect(editor.state.recipe?.nodes.map((n) => n.id)).toEqual(["n1"]);
    expect(editor.state.recipe?.edges).toEqual([]);
    expect(editor.state.selectedNodeId).toBeNull();
    expect(editor.state.positions.n2).toBeUndefined();
  });

  it("节点的位置：没有的自动排，拖过的记在浏览器里，重新打开还在", async () => {
    const { client, editor, storage } = await setup();
    await editor.addNode("vision.image_input");
    await editor.addNode("vision.threshold");
    await editor.connect("n1.image", "n2.image");
    await editor.addNode("vision.blob", { x: 700, y: 300 }); // 拖到画布上的位置
    expect(editor.state.positions.n1).toBeDefined();
    expect(editor.state.positions.n3).toEqual({ x: 700, y: 300 });
    editor.moveNode("n2", { x: 333, y: 44 });

    const again = createEditor(client, { storage });
    await again.init();
    await again.openRecipe(editor.state.recipe!.id);
    expect(again.state.positions.n2).toEqual({ x: 333, y: 44 });
    expect(again.state.positions.n3).toEqual({ x: 700, y: 300 });
  });

  it("点一下加的节点放在最右边那个的右侧", async () => {
    const { editor } = await setup();
    await editor.addNode("vision.image_input");
    await editor.addNode("vision.threshold");
    editor.moveNode("n1", { x: 500, y: 200 }); // 把第一个拖到右边去
    await editor.addNode("vision.blob");
    expect(editor.state.positions.n3).toEqual({ x: 710, y: 200 });
  });

  it("自动排：下游的节点在上游的右边", async () => {
    const { client, editor } = await setup();
    await editor.addNode("vision.image_input");
    await editor.addNode("vision.threshold");
    await editor.addNode("vision.blob");
    await editor.connect("n1.image", "n2.image");
    await editor.connect("n2.region", "n3.region");
    const fresh = createEditor(client); // 没有保存过位置
    await fresh.init();
    await fresh.openRecipe(editor.state.recipe!.id);
    const p = fresh.state.positions;
    expect(p.n1.x).toBeLessThan(p.n2.x);
    expect(p.n2.x).toBeLessThan(p.n3.x);
  });

  it("发布：成功给出版本号；被拒绝时显示原因；改过之后“已发布”的标记消失", async () => {
    const { editor } = await setup();
    await editor.publish();
    expect(editor.state.error).toContain("不能发布");
    expect(editor.state.publishedVersionId).toBeNull();
    await editor.addNode("vision.threshold");
    await editor.publish();
    expect(editor.state.publishedVersionId).toMatch(/@v1$/);
    await editor.setParam("n1", "value", 77);
    expect(editor.state.publishedVersionId).toBeNull();
  });
});
