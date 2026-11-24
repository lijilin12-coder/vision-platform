// 检测执行领域层测试：执行计划（拓扑排序）、Inspection 不变量 I1～I4、Executor、Judge。
// 用假的节点运行器代替真实算子，不链接 OpenCV。

#include <gtest/gtest.h>

#include "support/test_signatures.h"
#include "vision/execution/domain/executor.h"
#include "vision/execution/domain/plan.h"

using namespace vision;
using namespace vision::execution;

namespace {

// 假运行器：按算子 ID 产出固定结果；可以设定某个算子失败。
class FakeRunner final : public NodeRunner {
public:
    FakeRunner(OperatorSignature sig, int blobCount, bool fail) : sig_(std::move(sig)), blobs_(blobCount), fail_(fail) {}
    Result<NodeOutput> run(const PortValues&, const ParamMap&, const std::optional<Roi>&) override {
        // 失败的节点也“上报”过一个缺陷：它不应该出现在检测结果里（I5）
        if (fail_) return Result<NodeOutput>::failure("fake", "模拟失败");
        PortValues out;
        for (const auto& o : sig_.outputs) {
            switch (o.type) {
                case PortType::Image: out[o.name] = Image::create(1, 1, Image::Format::Gray8, {0}).value(); break;
                case PortType::Region:
                    out[o.name] = Region::fromMask(Image::create(1, 1, Image::Format::Gray8, {0}).value()).value();
                    break;
                case PortType::Blobs: out[o.name] = BlobList(static_cast<size_t>(blobs_)); break;
                case PortType::Judgement: out[o.name] = Judgement{blobs_ == 0, "缺陷 " + std::to_string(blobs_) + " 处"}; break;
                default: out[o.name] = 0.0;
            }
        }
        // 判定节点（有 Judgement 输出的）上报结果项：每个连通块一个缺陷，外加一个测量值
        NodeOutput result{std::move(out), {}};
        if (sig_.contributesToVerdict()) {
            for (int i = 0; i < blobs_; ++i) {
                Defect d;
                d.category = "亮斑";
                d.area = 10.0 * (i + 1);
                result.items.defects.push_back(d);
            }
            Measurement m;
            m.name = "个数";
            m.value = blobs_;
            result.items.measurements.push_back(m);
        }
        return result;
    }

private:
    OperatorSignature sig_;
    int blobs_;
    bool fail_;
};

class FakeFactory final : public NodeRunnerFactory {
public:
    int blobs = 0;
    std::string failOperator;
    Result<std::shared_ptr<NodeRunner>> create(const PlanNode& node) override {
        const OperatorSignature& sig = node.signature;
        return std::shared_ptr<NodeRunner>(std::make_shared<FakeRunner>(sig, blobs, sig.id == failOperator));
    }
};

// 故意打乱节点顺序，检验拓扑排序
PlanSpec pipelineSpec() {
    PlanSpec s;
    s.recipeVersionId = "r@v1";
    s.nodes = {{"n5", test::judge(), {}, std::nullopt, {}},
               {"n3", test::threshold(), test::threshold().defaultParams(), std::nullopt, {}},
               {"n1", test::imageInput(), {}, std::nullopt, {}},
               {"n4", test::blob(), {}, std::nullopt, {}},
               {"n2", test::gray(), {}, std::nullopt, {}}};
    s.edges = {{"n1", "image", "n2", "image"},
               {"n2", "image", "n3", "image"},
               {"n3", "region", "n4", "region"},
               {"n4", "blobs", "n5", "blobs"}};
    return s;
}

Image frame() { return Image::create(1, 1, Image::Format::Gray8, {7}).value(); }

}  // namespace

TEST(ExecutionPlanner, OrdersNodesTopologically) {
    FakeFactory f;
    auto plan = ExecutionPlanner(f).plan(pipelineSpec());
    ASSERT_TRUE(plan.ok()) << plan.error().message;
    EXPECT_EQ(plan.value().nodeOrder(), (std::vector<std::string>{"n1", "n2", "n3", "n4", "n5"}));
    EXPECT_TRUE(plan.value().steps()[0].isImageInput);
}

TEST(ExecutionPlanner, RejectsCycleAndUnboundInput) {
    FakeFactory f;
    auto spec = pipelineSpec();
    spec.edges.push_back({"n5", "judgement", "n2", "image"});  // 类型也不兼容
    EXPECT_FALSE(ExecutionPlanner(f).plan(spec).ok());

    spec = pipelineSpec();
    spec.edges.pop_back();  // n5 的输入没连
    EXPECT_EQ(ExecutionPlanner(f).plan(spec).error().code, "execution.unbound_input");
}

TEST(Inspection, I1_RecordsInPlannedOrder) {
    auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", {"n1", "n2"});
    EXPECT_FALSE(i.recordNode({"n2", NodeStatus::Succeeded, 0, ""}).ok());
    EXPECT_TRUE(i.recordNode({"n1", NodeStatus::Succeeded, 0, ""}).ok());
}

TEST(Inspection, I2_FailedNodeRequiresError) {
    auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", {"n1"});
    ASSERT_TRUE(i.recordNode({"n1", NodeStatus::Failed, 0, "坏了"}).ok());
    EXPECT_FALSE(i.complete(Verdict{VerdictKind::Ok, {}}).ok());
    EXPECT_TRUE(i.complete(Verdict{VerdictKind::Error, {}}).ok());
}

// I5：结果项只能来自执行成功的节点
TEST(Inspection, I5_ResultItemsMustComeFromSucceededNodes) {
    const auto withDefectFrom = [](const std::string& nodeId) {
        ResultItems items;
        Defect d;
        d.nodeId = nodeId;
        items.defects.push_back(d);
        return items;
    };
    {
        auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", {"n1", "n2"});
        ASSERT_TRUE(i.recordNode({"n1", NodeStatus::Succeeded, 0, ""}).ok());
        ASSERT_TRUE(i.recordNode({"n2", NodeStatus::Failed, 0, "坏了"}).ok());
        // 来自失败的节点、来自不存在的节点、没有写是谁报的：都不行
        for (const char* bad : {"n2", "n9", ""}) {
            const auto st = i.complete(Verdict{VerdictKind::Error, {}}, withDefectFrom(bad));
            ASSERT_FALSE(st.ok()) << bad;
            EXPECT_EQ(st.error().code, "execution.result_inconsistent");
        }
        EXPECT_FALSE(i.completed());  // 被拒绝的完成不留痕迹
        // 成功的节点报的可以留下，哪怕这次检测整体是 ERROR
        ASSERT_TRUE(i.complete(Verdict{VerdictKind::Error, {}}, withDefectFrom("n1")).ok());
        EXPECT_EQ(i.result().defects.size(), 1u);
    }
    {
        // 测量值、位姿同样检查；完成的事件里带着结果项
        auto i = Inspection::start("i-2", InspectionPurpose::Production, "r@v1", {"n1"});
        ASSERT_TRUE(i.recordNode({"n1", NodeStatus::Succeeded, 0, ""}).ok());
        ResultItems items;
        Measurement m;
        m.nodeId = "n7";
        items.measurements.push_back(m);
        EXPECT_FALSE(i.complete(Verdict{}, items).ok());
        items.measurements[0].nodeId = "n1";
        PoseResult p;
        p.nodeId = "n7";
        items.poses.push_back(p);
        EXPECT_FALSE(i.complete(Verdict{}, items).ok());
        items.poses[0].nodeId = "n1";
        ASSERT_TRUE(i.complete(Verdict{}, items).ok());
        const auto events = i.pullEvents();
        ASSERT_EQ(events.size(), 1u);
        const auto* completed = dynamic_cast<const InspectionCompleted*>(events[0].get());
        ASSERT_NE(completed, nullptr);
        EXPECT_EQ(completed->result.measurements.size(), 1u);
        EXPECT_EQ(completed->result.poses.size(), 1u);
    }
}

TEST(Inspection, I3_CompleteOnlyOnceAndThenImmutable) {
    auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", {"n1"});
    EXPECT_FALSE(i.complete(Verdict{}).ok()) << "还有节点没执行，不能完成";
    ASSERT_TRUE(i.recordNode({"n1", NodeStatus::Succeeded, 0, ""}).ok());
    ASSERT_TRUE(i.complete(Verdict{}).ok());
    EXPECT_FALSE(i.complete(Verdict{}).ok());
    EXPECT_FALSE(i.recordNode({"n1", NodeStatus::Succeeded, 0, ""}).ok());
}

TEST(Inspection, I4_NgNeedsFailingReason) {
    auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", {"n1"});
    ASSERT_TRUE(i.recordNode({"n1", NodeStatus::Succeeded, 0, ""}).ok());
    EXPECT_FALSE(i.complete(Verdict{VerdictKind::Ng, {{"n1", true, "都合格"}}}).ok());
    EXPECT_TRUE(i.complete(Verdict{VerdictKind::Ng, {{"n1", false, "缺陷 1 处"}}}).ok());
}

TEST(Inspection, DebugRunDoesNotPublishCompletedEvent) {
    auto prod = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", {});
    ASSERT_TRUE(prod.complete(Verdict{}).ok());
    EXPECT_EQ(prod.pullEvents().size(), 1u);

    auto debug = Inspection::start("i-2", InspectionPurpose::Debug, "r@draft", {});
    ASSERT_TRUE(debug.complete(Verdict{}).ok());
    EXPECT_TRUE(debug.pullEvents().empty());
}

TEST(Executor, OkWhenNoDefects) {
    FakeFactory f;
    auto plan = ExecutionPlanner(f).plan(pipelineSpec()).value();
    auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", plan.nodeOrder());
    ASSERT_TRUE(Executor().run(plan, frame(), i).ok());
    EXPECT_EQ(i.verdict().kind, VerdictKind::Ok);
    EXPECT_EQ(i.nodeExecutions().size(), 5u);
}

TEST(Executor, NgWithReasonWhenDefectsFound) {
    FakeFactory f;
    f.blobs = 3;
    auto plan = ExecutionPlanner(f).plan(pipelineSpec()).value();
    auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", plan.nodeOrder());
    ASSERT_TRUE(Executor().run(plan, frame(), i).ok());
    EXPECT_EQ(i.verdict().kind, VerdictKind::Ng);
    ASSERT_EQ(i.verdict().reasons.size(), 1u);
    EXPECT_EQ(i.verdict().reasons[0].text, "缺陷 3 处");
    // 结果项：判定节点上报的 3 个缺陷和 1 个测量值进了检测结果，记着是哪个节点报的
    ASSERT_EQ(i.result().defects.size(), 3u);
    EXPECT_EQ(i.result().defects[0].nodeId, plan.nodeOrder().back());
    EXPECT_EQ(i.result().defects[2].area, 30.0);
    ASSERT_EQ(i.result().measurements.size(), 1u);
    EXPECT_EQ(i.result().measurements[0].nodeId, plan.nodeOrder().back());
}

// 判定节点被跳过（上游失败）时，它没有运行，自然也没有结果项
TEST(Executor, SkippedNodesContributeNoResultItems) {
    FakeFactory f;
    f.blobs = 3;
    f.failOperator = "test.threshold";
    auto plan = ExecutionPlanner(f).plan(pipelineSpec()).value();
    auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", plan.nodeOrder());
    ASSERT_TRUE(Executor().run(plan, frame(), i).ok());
    EXPECT_EQ(i.verdict().kind, VerdictKind::Error);
    EXPECT_TRUE(i.result().empty());
}

TEST(Executor, FailureSkipsDownstreamAndGivesError) {
    FakeFactory f;
    f.failOperator = "test.threshold";
    auto plan = ExecutionPlanner(f).plan(pipelineSpec()).value();
    auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", plan.nodeOrder());
    ASSERT_TRUE(Executor().run(plan, frame(), i).ok());
    const auto& ex = i.nodeExecutions();
    EXPECT_EQ(ex[2].status, NodeStatus::Failed);
    EXPECT_EQ(ex[3].status, NodeStatus::Skipped);
    EXPECT_EQ(ex[4].status, NodeStatus::Skipped);
    EXPECT_EQ(i.verdict().kind, VerdictKind::Error);
}

TEST(Executor, MissingFrameIsError) {
    FakeFactory f;
    auto plan = ExecutionPlanner(f).plan(pipelineSpec()).value();
    auto i = Inspection::start("i-1", InspectionPurpose::Production, "r@v1", plan.nodeOrder());
    ASSERT_TRUE(Executor().run(plan, Image{}, i).ok());
    EXPECT_EQ(i.verdict().kind, VerdictKind::Error);
}
