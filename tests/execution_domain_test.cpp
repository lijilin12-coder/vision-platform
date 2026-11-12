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
    Result<PortValues> run(const PortValues&, const ParamMap&, const std::optional<Roi>&) override {
        if (fail_) return Result<PortValues>::failure("fake", "模拟失败");
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
        return out;
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
    Result<std::shared_ptr<NodeRunner>> create(const OperatorSignature& sig) override {
        return std::shared_ptr<NodeRunner>(std::make_shared<FakeRunner>(sig, blobs, sig.id == failOperator));
    }
};

// 故意打乱节点顺序，检验拓扑排序
PlanSpec pipelineSpec() {
    PlanSpec s;
    s.recipeVersionId = "r@v1";
    s.nodes = {{"n5", test::judge(), {}, std::nullopt},
               {"n3", test::threshold(), test::threshold().defaultParams(), std::nullopt},
               {"n1", test::imageInput(), {}, std::nullopt},
               {"n4", test::blob(), {}, std::nullopt},
               {"n2", test::gray(), {}, std::nullopt}};
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
