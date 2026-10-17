#include "cordite/assets/RenderTarget.h"

#include "cordite/core/Document.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace cordite {
namespace {

constexpr DocumentFormat kRenderTargetFormat{.name = "cordite.target", .version = 1, .kind = "render target"};

constexpr std::array<std::string_view, 2> kHeaderMembers = {"format", "version"};

TypeInfo describeRenderTarget() {
    return TypeBuilder<RenderTargetData>("RenderTarget")
        .property("width", &RenderTargetData::width,
                  {.description = "How many pixels wide the picture is",
                   .minimum = 1.0,
                   .maximum = static_cast<double>(kMaxRenderTargetSize)})
        .property("height", &RenderTargetData::height,
                  {.description = "How many pixels high the picture is",
                   .minimum = 1.0,
                   .maximum = static_cast<double>(kMaxRenderTargetSize)})
        .build();
}

} // namespace

const TypeInfo& renderTargetTypeInfo() {
    static const TypeInfo kType = describeRenderTarget();
    return kType;
}

Result<RenderTargetData> parseRenderTargetFile(const Json& document) {
    CORDITE_TRY(readDocumentHeader(document, kRenderTargetFormat));
    Json properties = Json::object();
    for (const auto& [key, value] : document.items()) {
        if (std::ranges::find(kHeaderMembers, std::string_view(key)) == kHeaderMembers.end()) {
            properties[key] = value;
        }
    }
    CORDITE_TRY_ASSIGN(const PropertyPatch patch, renderTargetTypeInfo().parse(properties));
    RenderTargetData target;
    patch.applyTo(&target);
    return target;
}

Json serializeRenderTarget(const RenderTargetData& target) {
    Json document = makeDocument(kRenderTargetFormat);
    document.update(renderTargetTypeInfo().toJson(&target));
    return document;
}

} // namespace cordite
