// Copyright (C) 2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "dnnl_fullyconnected_primitive.hpp"

#include <oneapi/dnnl/dnnl_types.h>

#include <common/primitive_attr.hpp>
#include <common/primitive_desc_iface.hpp>
#include <common/primitive_iface.hpp>
#include <iostream>
#include <memory>
#include <oneapi/dnnl/dnnl.hpp>
#include <oneapi/dnnl/dnnl_common.hpp>

#include "cpu/x64/cpu_isa_traits.hpp"
#include "cpu_memory.h"
#include "cpu_types.h"
#include "dnnl_extension_utils.h"
#include "dnnl_postops_composer.h"
#include "memory_desc/cpu_memory_desc.h"
#include "memory_desc/cpu_memory_desc_utils.h"
#include "memory_desc/dnnl_memory_desc.h"
#include "nodes/executors/dnnl/dnnl_shape_agnostic_data.hpp"
#include "nodes/executors/executor.hpp"
#include "nodes/executors/fullyconnected_config.hpp"
#include "nodes/executors/memory_arguments.hpp"
#include "utils/debug_capabilities.h"

namespace ov {
namespace intel_cpu {

using namespace dnnl;
using namespace ov::element;
using namespace executor;

// @todo rewrite using hash_builder
size_t DnnlFCPrimitive::Key::hash() const {
    using namespace dnnl::impl;
    using namespace dnnl::impl::primitive_hashing;

    size_t seed = 0;

    for (const auto& ptr : {src, wei, bias, dst}) {
        if (ptr) {
            seed = hash_combine(seed, get_md_hash(*ptr->getDnnlDesc().get()));
        }
    }

    seed = hash_combine(seed, get_attr_hash(*attr.get()));
    seed = hash_combine(seed, sparseWeights);
    seed = hash_combine(seed, transposedWeights);

    return seed;
}

bool DnnlFCPrimitive::Key::operator==(const Key& rhs) const {
    bool result = true;

    if (src != rhs.src) {
        result = result && src && rhs.src && src->getDnnlDesc() == rhs.src->getDnnlDesc();
    }
    if (wei != rhs.wei) {
        result = result && wei && rhs.wei && wei->getDnnlDesc() == rhs.wei->getDnnlDesc();
    }
    if (bias != rhs.bias) {
        result = result && bias && rhs.bias && bias->getDnnlDesc() == rhs.bias->getDnnlDesc();
    }
    if (dst != rhs.dst) {
        result = result && dst && rhs.dst && dst->getDnnlDesc() == rhs.dst->getDnnlDesc();
    }

    result = result && *attr.get() == *rhs.attr.get() && sparseWeights == rhs.sparseWeights &&
             transposedWeights == rhs.transposedWeights;

    return result;
}

std::shared_ptr<DnnlFCPrimitive> DnnlFCPrimitive::create(const MemoryArgs& memory,
                                                         const FCAttrs& attrs,
                                                         const ExecutorContext::CPtr context,
                                                         const DnnlShapeAgnosticDataPtr& shapeAgnosticData) {
    const auto& srcDesc = MemoryDescUtils::convertToDnnlMemoryDesc(memory.at(ARG_SRC)->getDescPtr());
    const auto& weiDesc = MemoryDescUtils::convertToDnnlMemoryDesc(memory.at(ARG_WEI)->getDescPtr());
    const DnnlMemoryDescPtr biaDesc = memory.at(ARG_BIAS)->getDescPtr()->getCurrentMemSize() != 0
                                          ? MemoryDescUtils::convertToDnnlMemoryDesc(memory.at(ARG_BIAS)->getDescPtr())
                                          : DnnlExtensionUtils::makeDescriptor(dnnl::memory::desc{});
    const auto& dstDesc = MemoryDescUtils::convertToDnnlMemoryDesc(memory.at(ARG_DST)->getDescPtr());

    Key dnnlFCKey{
        srcDesc,
        weiDesc,
        biaDesc,
        dstDesc,
        shapeAgnosticData->primAttrs.attr,
        attrs.sparseWeights,
        attrs.weightsNonTransposed,
    };

    auto builder = [&context](const Key& dnnlKey) {
        return std::make_shared<DnnlFCPrimitive>(dnnlKey, context->getEngine(), context->getImplPriorities());
    };

    auto runtimeCache = context->getRuntimeCache();
    const auto result = runtimeCache->getOrCreate(dnnlFCKey, builder);
    const auto& primitive = result.first;
    assert(primitive);

    return primitive;
}

bool DnnlFCPrimitive::useWeightsDecompressionImpl(const ov::element::Type inputType,
                                                  const ov::element::Type weightsType) {
    return dnnl::impl::cpu::x64::mayiuse(dnnl::impl::cpu::x64::avx2) && one_of(inputType, f32, bf16) &&
           one_of(weightsType, u8, nf4, u4, i4);
}

template <typename T>
static std::vector<T> normalizeDimsTo2D(const std::vector<T>& dims) {
    return {std::accumulate(dims.begin(), dims.end() - 1, (T)1, std::multiplies<T>()), dims[dims.size() - 1]};
}

static DnnlPrimitiveAttrs createPrimitiveAttrs(const FCAttrs& attrs,
                                               const PostOps& postOps,
                                               const MemoryArgs& memory,
                                               ExecutorContext::CPtr context) {
    const auto& srcDesc = memory.at(ARG_SRC)->getDescPtr();
    const auto& weiDesc = memory.at(ARG_WEI)->getDescPtr();
    const auto& dstDesc = memory.at(ARG_DST)->getDescPtr();

    const auto& originalDims = dstDesc->getShape().getMinDims();
    const auto& dims = normalizeDimsTo2D(originalDims);

    auto isINT8 =
        one_of(srcDesc->getPrecision(), ov::element::u8, ov::element::i8) && weiDesc->getPrecision() == ov::element::i8;
    auto outputDataType = DnnlExtensionUtils::ElementTypeToDataType(dstDesc->getPrecision());

    DnnlPostOpsComposer dnnlpoc(postOps,
                                context->getEngine(),
                                dims,
                                dims.size() - 1,
                                isINT8,
                                1 << 0,
                                attrs.dequantizationScales,
                                attrs.withBias,
                                outputDataType);

    if (attrs.decompressionMultiplyPtr)
        dnnlpoc.appendDecompressionScales(attrs.decompressionMultiplyPtr, !attrs.weightsNonTransposed);
    if (attrs.decompressionSubtractPtr)
        dnnlpoc.appendDecompressionZeroPoints(attrs.decompressionSubtractPtr,
                                              !attrs.weightsNonTransposed);

    return dnnlpoc.compose();
}

static dnnl::memory::desc normalizeDescriptor(const dnnl::memory::desc& desc) {
    const auto& dims = desc.get_dims();

    if (dims.size() > 2)
        return desc.reshape(normalizeDimsTo2D(dims));

    return desc;
}

static dnnl::inner_product_forward::primitive_desc createDescriptorInternal(const dnnl::memory::desc& inputDesc,
                                                                            const dnnl::memory::desc& weightDesc,
                                                                            const dnnl::memory::desc& biasDesc,
                                                                            const dnnl::memory::desc& outputDesc,
                                                                            const dnnl::primitive_attr& attr,
                                                                            const dnnl::engine& engine,
                                                                            const bool useSparseWeights,
                                                                            const bool useWeightsDecompression) {
    const auto normalizedInputDesc = normalizeDescriptor(inputDesc);
    const auto normalizedOutputDesc = normalizeDescriptor(outputDesc);

    const auto indt = normalizedInputDesc.get_data_type();
    auto wdt = indt;

    if (useWeightsDecompression) {
        wdt = weightDesc.get_data_type();
    } else if (indt == dnnl::memory::data_type::u8 || indt == dnnl::memory::data_type::s8) {
        wdt = memory::data_type::s8;
    }

    const dnnl::memory::desc weightsDesc =
        useSparseWeights ? dnnl::memory::desc().sparse_desc(weightDesc.get_dims(), wdt)
                         : dnnl::memory::desc(weightDesc.get_dims(), wdt, memory::format_tag::any);

    return dnnl::inner_product_forward::primitive_desc(engine,
                                                       dnnl::prop_kind::forward_inference,
                                                       normalizedInputDesc,
                                                       weightsDesc,
                                                       biasDesc,
                                                       normalizedOutputDesc,
                                                       attr);
}

static primitive_desc createPrimitiveDesc(const dnnl::memory::desc& inputDesc,
                                          const dnnl::memory::desc& weightDesc,
                                          const dnnl::memory::desc& biasDesc,
                                          const dnnl::memory::desc& outputDesc,
                                          const dnnl::primitive_attr& attr,
                                          const dnnl::engine& engine,
                                          const std::vector<impl_desc_type>& implPriorities,
                                          const bool useSparseWeights,
                                          const bool useWeightsDecompression) {
    auto prim_desc = createDescriptorInternal(inputDesc,
                                              weightDesc,
                                              biasDesc,
                                              outputDesc,
                                              attr,
                                              engine,
                                              useSparseWeights,
                                              useWeightsDecompression);
    OPENVINO_ASSERT(prim_desc, "Failed to create inner_product primitive descriptor");
    auto first_desc = dnnl::inner_product_forward::primitive_desc(prim_desc.get());

    const bool found = DnnlExtensionUtils::find_implementation(prim_desc, [&](impl_desc_type implType) {
        return contains(implPriorities, implType);
    });

    if (found)
        return std::move(prim_desc);

    return std::move(first_desc);
}

static VectorDims makeDummyInputDims(const Shape& inShape, const Shape& wShape) {
    const auto& weightDims = wShape.getStaticDims();

    auto inMinDims = inShape.getMinDims();
    auto inMaxDims = inShape.getMaxDims();
    inMinDims.back() = weightDims.back();
    inMaxDims.back() = weightDims.back();

    return MemoryDescUtils::makeDummyShape(Shape(inMinDims, inMaxDims)).getStaticDims();
}

static VectorDims makeDummyOutputDims(const VectorDims& inShape, const VectorDims& wShape, const size_t out_rank) {
    size_t activationRank = inShape.size();
    size_t channelRank = wShape.size() - 1;
    // activation   weight    output_shape
    // NCHW         CoCHW     NCo
    // TNC          CoC       TNCo
    // NC           CoC       NCo
    VectorDims outputShape(out_rank, 1);
    // set Co
    outputShape.back() = wShape[0];
    // set batch dims
    size_t batchRank = activationRank - channelRank;
    size_t startIdx = out_rank - batchRank - 1;
    for (size_t i = 0; i < batchRank; i++) {
        outputShape[i + startIdx] = inShape[i];
    }

    return outputShape;
}

DnnlShapeAgnosticDataPtr DnnlFCPrimitive::createShapeAgnosticData(const FCAttrs& attrs,
                                                                  const PostOps& postOps,
                                                                  const MemoryArgs& memory,
                                                                  const ExecutorContext::CPtr context,
                                                                  const bool cacheWeights) {
    DEBUG_LOG("Creating shape agnostic data");
    auto srcDesc = memory.at(ARG_SRC)->getDescPtr();
    const auto& weiDesc = memory.at(ARG_WEI)->getDescPtr();
    auto dstDesc = memory.at(ARG_DST)->getDescPtr();

    const auto postOpData = createPrimitiveAttrs(attrs, postOps, memory, context);

    if (!cacheWeights)
        return std::make_shared<DnnlShapeAgnosticData>(postOpData);

    if (srcDesc->getShape().isDynamic()) {
        const auto& inShape = srcDesc->getShape();
        const auto& wShape = weiDesc->getShape();
        const auto& inDymmyDims = makeDummyInputDims(inShape, wShape);
        srcDesc = srcDesc->cloneWithNewDims(inDymmyDims);
        const auto& outDymmyDims =
            makeDummyOutputDims(inDymmyDims, wShape.getStaticDims(), dstDesc->getShape().getRank());
        dstDesc = dstDesc->cloneWithNewDims(outDymmyDims);
    }

    const dnnl::memory::desc srcDnnlDesc = MemoryDescUtils::convertToDnnlMemoryDesc(srcDesc)->getDnnlDesc();
    const dnnl::memory::desc weiDnnlDesc = MemoryDescUtils::convertToDnnlMemoryDesc(weiDesc)->getDnnlDesc();
    const dnnl::memory::desc dstDnnlDesc = MemoryDescUtils::convertToDnnlMemoryDesc(dstDesc)->getDnnlDesc();
    const auto useSparseWeights = attrs.sparseWeights;
    const auto useWeightsDecompression = useWeightsDecompressionImpl(srcDesc->getPrecision(), weiDesc->getPrecision());

    const dnnl::memory::desc biaDnnlDesc =
        memory.at(ARG_BIAS)->getDescPtr()->getCurrentMemSize() != 0
            ? MemoryDescUtils::convertToDnnlMemoryDesc(memory.at(ARG_BIAS)->getDescPtr())->getDnnlDesc()
            : dnnl::memory::desc{};

    const auto primDesc = createPrimitiveDesc(srcDnnlDesc,
                                              weiDnnlDesc,
                                              biaDnnlDesc,
                                              dstDnnlDesc,
                                              postOpData.attr,
                                              context->getEngine(),
                                              context->getImplPriorities(),
                                              useSparseWeights,
                                              useWeightsDecompression);

    const auto weightsDesc = DnnlExtensionUtils::makeDescriptor(primDesc.weights_desc());
    auto originalWeightsDesc = MemoryDescUtils::convertToDnnlMemoryDesc(weiDesc);
    if (attrs.weightsNonTransposed)
        originalWeightsDesc = utils::makeTransposedWeightDescriptor(originalWeightsDesc, weightsDesc);

    // ignore the result since we just need to put the packed weights into the cache
    (void)utils::prepareWeightsMemory(originalWeightsDesc,
                                      weightsDesc,
                                      memory.at(ARG_WEI),
                                      context);

    return std::make_shared<DnnlShapeAgnosticData>(postOpData);
}

static impl_desc_type implTypeFromPrimDesc(const dnnl::primitive_desc primDesc) {
    const auto implType = parse_impl_name(primDesc.impl_info_str());
    if (implType == ov::intel_cpu::brgemm_avx512_amx &&
        primDesc.weights_desc().get_format_kind() == memory::format_kind::sparsed) {
        return ov::intel_cpu::brgemm_sparse_avx512_amx;
    }

    return implType;
}

DnnlFCPrimitive::DnnlFCPrimitive(const Key& key,
                                 const dnnl::engine& engine,
                                 const std::vector<impl_desc_type>& implPriorities)
    : m_stream(dnnl::stream(engine)),
      m_primDesc(createPrimitiveDesc(key.src->getDnnlDesc(),
                                     key.wei->getDnnlDesc(),
                                     key.bias->getDnnlDesc(),
                                     key.dst->getDnnlDesc(),
                                     key.attr,
                                     engine,
                                     implPriorities,
                                     key.sparseWeights,
                                     useWeightsDecompressionImpl(key.src->getPrecision(), key.wei->getPrecision()))),
      m_implType(implTypeFromPrimDesc(m_primDesc)),
      m_srcDesc(DnnlExtensionUtils::makeDescriptor(m_primDesc.src_desc())),
      m_weiDesc(DnnlExtensionUtils::makeDescriptor(m_primDesc.weights_desc())),
      m_dstDesc(DnnlExtensionUtils::makeDescriptor(m_primDesc.dst_desc())),
      m_scratchPadDesc(DnnlExtensionUtils::makeDescriptor(m_primDesc.scratchpad_desc())),
      m_prim(primitive(m_primDesc)) {}

void DnnlFCPrimitive::execute(const dnnl_primitive_args& primArgs) const {
    m_prim.execute(m_stream, primArgs);
}

}  // namespace intel_cpu
}  // namespace ov
