// 算子目录测试：注册规则（事件 A1 / A2）与主版本号检查（D5-1）。

#include <gtest/gtest.h>

#include "vision/catalog/api.h"

using namespace vision;
using namespace vision::catalog;

namespace {

class Dummy final : public Operator {
public:
    Status execute(OpContext&) override { return Status::success(); }
};

OperatorMeta meta(const std::string& id, int major = 1) {
    OperatorMeta m;
    m.signature.id = id;
    m.signature.majorVersion = major;
    m.signature.outputs = {{"out", PortType::Number, false}};
    return m;
}

OperatorFactory factory() {
    return [] { return std::unique_ptr<Operator>(new Dummy()); };
}

}  // namespace

TEST(OperatorCatalog, RegistersAndFindsById) {
    OperatorCatalog c;
    ASSERT_TRUE(c.registerOperator(meta("test.a"), factory()).ok());
    ASSERT_TRUE(c.registerOperator(meta("test.b"), factory()).ok());
    // 回归测试：曾经因为 ID 在被 move 之后才作为键使用，所有算子都存成了空字符串
    EXPECT_TRUE(c.find("test.a").has_value());
    EXPECT_TRUE(c.find("test.b").has_value());
    EXPECT_EQ(c.list().size(), 2u);
}

TEST(OperatorCatalog, RejectsDuplicateAndInvalid) {
    OperatorCatalog c;
    ASSERT_TRUE(c.registerOperator(meta("test.a"), factory()).ok());
    EXPECT_EQ(c.registerOperator(meta("test.a"), factory()).error().code, "catalog.duplicate_id");
    EXPECT_EQ(c.registerOperator(meta("noDot"), factory()).error().code, "catalog.invalid_signature");
    EXPECT_EQ(c.registerOperator(meta("test.v0", 0), factory()).error().code, "catalog.invalid_signature");

    auto bad = meta("test.badparam");
    bad.signature.params = {{"x", ParamType::Number, 50.0, 0.0, 10.0, {}, ""}};  // 默认值超出范围
    EXPECT_EQ(c.registerOperator(bad, factory()).error().code, "catalog.invalid_signature");
}

TEST(OperatorCatalog, CreateChecksMajorVersion) {
    OperatorCatalog c;
    ASSERT_TRUE(c.registerOperator(meta("test.a", 2), factory()).ok());
    EXPECT_TRUE(c.create("test.a", 2).ok());
    EXPECT_EQ(c.create("test.a", 1).error().code, "catalog.major_mismatch");
    EXPECT_EQ(c.create("test.zzz", 1).error().code, "catalog.not_found");
}
