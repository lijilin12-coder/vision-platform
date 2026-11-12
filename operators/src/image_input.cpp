// 图像入口：检测时由执行引擎直接把图像送到它的输出端口，不会调用 execute()。
#include "vision/catalog/api.h"

namespace vision::operators {

class ImageInputOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = kImageInputOperatorId;
        m.signature.majorVersion = 1;
        m.signature.outputs = {{"image", PortType::Image, false}};
        m.displayName = "图像输入";
        m.category = "输入";
        m.description = "检测图像的入口";
        return m;
    }

    Status execute(catalog::OpContext&) override {
        return Status::failure("operator.image_input", "图像入口节点由执行引擎直接提供图像");
    }
};

}  // namespace vision::operators

using vision::operators::ImageInputOp;
VISION_REGISTER_OPERATOR(ImageInputOp)
