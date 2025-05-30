//===------------------------------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "torch-mlir/Conversion/TorchOnnxToTorch/Patterns.h"
#include "torch-mlir/Conversion/TorchOnnxToTorch/Utils.h"
#include "torch-mlir/Dialect/Torch/IR/TorchOps.h"
#include "torch-mlir/Dialect/Torch/Utils/Utils.h"
#include <numeric>

using namespace mlir;
using namespace mlir::torch;
using namespace mlir::torch::onnx_c;

void mlir::torch::onnx_c::populateComMicrosoftDomain(
    OnnxCustomOpConversionPattern &patterns) {
  patterns.onOp(
      "RotaryEmbedding", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        Location loc = binder.getLoc();
        int64_t interleaved, isPackedBatching, numHeads, rotaryEmbeddingDim;
        float scale;
        Value input, positionIds, cosCache, sinCache;
        if (binder.tensorOperandAtIndex(input, 0) ||
            binder.tensorOperandAtIndex(positionIds, 1) ||
            binder.tensorOperandAtIndex(cosCache, 2) ||
            binder.tensorOperandAtIndex(sinCache, 3) ||
            binder.s64IntegerAttr(interleaved, "interleaved", 0) ||
            binder.s64IntegerAttr(isPackedBatching, "is_packed_batching", 0) ||
            binder.s64IntegerAttr(numHeads, "num_heads", 0) ||
            binder.s64IntegerAttr(rotaryEmbeddingDim, "rotary_embedding_dim",
                                  0) ||
            binder.f32FloatAttr(scale, "scale", 1.0)) {
          return rewriter.notifyMatchFailure(binder.op,
                                             "Failed to get required inputs");
        }

        Torch::ValueTensorType resultType;
        if (binder.tensorResultType(resultType)) {
          return rewriter.notifyMatchFailure(binder.op,
                                             "result type bind failure");
        }

        Value cstInterleaved = rewriter.create<Torch::ConstantIntOp>(
            loc, rewriter.getI64IntegerAttr(interleaved));
        Value cstIsPackedBatching = rewriter.create<Torch::ConstantIntOp>(
            loc, rewriter.getI64IntegerAttr(isPackedBatching));
        Value cstNumHeads = rewriter.create<Torch::ConstantIntOp>(
            loc, rewriter.getI64IntegerAttr(numHeads));
        Value cstRotaryEmbeddingDim = rewriter.create<Torch::ConstantIntOp>(
            loc, rewriter.getI64IntegerAttr(rotaryEmbeddingDim));
        Value cstScale = rewriter.create<Torch::ConstantFloatOp>(
            loc, rewriter.getF64FloatAttr(scale));

        rewriter.replaceOpWithNewOp<Torch::OnnxVariantRotaryEmbeddingOp>(
            binder.op, resultType, input, positionIds, cosCache, sinCache,
            cstInterleaved, cstIsPackedBatching, cstNumHeads,
            cstRotaryEmbeddingDim, cstScale);
        return success();
      });
  patterns.onOp(
      "GroupQueryAttention", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        SmallVector<Value> operands;
        SmallVector<Type> resultTypes;
        int64_t doRotary, kvNumHeads, localWindowSize, numHeads,
            rotaryInterleaved, smoothSoftmax;
        float scale, softcap;
        if (binder.tensorOperandsList(operands))
          return rewriter.notifyMatchFailure(binder.op,
                                             "operands bind failure");

        if (binder.tensorResultTypes(resultTypes))
          return rewriter.notifyMatchFailure(binder.op,
                                             "result types bind failure");

        if (resultTypes.size() != 3)
          return rewriter.notifyMatchFailure(binder.op,
                                             "expected 3 result types");

        if (binder.s64IntegerAttr(doRotary, "do_rotary") ||
            binder.s64IntegerAttr(kvNumHeads, "kv_num_heads") ||
            binder.s64IntegerAttr(localWindowSize, "local_window_size", -1) ||
            binder.s64IntegerAttr(numHeads, "num_heads") ||
            binder.s64IntegerAttr(rotaryInterleaved, "rotary_interleaved") ||
            binder.f32FloatAttr(scale, "scale") ||
            binder.s64IntegerAttr(smoothSoftmax, "smooth_softmax") ||
            binder.f32FloatAttr(softcap, "softcap"))
          return rewriter.notifyMatchFailure(binder.op,
                                             "op attributes bind failure");

        // This lowering excepts input operands to be either 7 or 9 based on the
        // `do_rotary` attribute. If it's false, then the input operands can be
        // 7 but if it's true then the operands has to be 9 including cos_cache
        // and sin_cache for rotary_embedding.
        if (!((operands.size() == 9) || (!doRotary && operands.size() == 7)))
          return rewriter.notifyMatchFailure(
              binder.op, "Unimplemented:  excepted input operands to be either "
                         "7 or 9 based on the `do_rotary` attribute");

        if (kvNumHeads == 0)
          return rewriter.notifyMatchFailure(
              binder.op,
              "kv_num_heads is a required attribute and should be non-zero");

        if (localWindowSize != -1)
          return rewriter.notifyMatchFailure(
              binder.op,
              "Unimplemented: local_window_size attribute is not supported, "
              "hence it should have default value equal to -1");

        if (numHeads == 0)
          return rewriter.notifyMatchFailure(
              binder.op,
              "num_heads is a required attribute and should be non-zero");

        if (smoothSoftmax != 0)
          return rewriter.notifyMatchFailure(
              binder.op,
              "Unimplemented: smooth_softmax attribute is not supported, hence "
              "it should have default value equal to 0");

        if (softcap != 0.0f)
          return rewriter.notifyMatchFailure(
              binder.op, "Unimplemented: softcap attribute is not supported, "
                         "hence it should have default value equal to 0.0");

        // TODO: Add support for packed_qkv.

        Location loc = binder.getLoc();
        MLIRContext *context = binder.op->getContext();
        Value query = operands[0];
        Value key = operands[1];
        Value value = operands[2];
        Value pastKey = operands[3];
        Value pastValue = operands[4];
        Value seqlensK = operands[5];
        Value totalSequenceLength = operands[6];
        Value cosCache, sinCache;
        if (doRotary) {
          cosCache = operands[7];
          sinCache = operands[8];
        }

        Torch::ValueTensorType queryType =
            cast<Torch::ValueTensorType>(query.getType());
        if (!(queryType.hasSizes() && queryType.areAllSizesKnown()))
          return rewriter.notifyMatchFailure(
              binder.op,
              "Expected `query` input to have statically known sizes");

        SmallVector<int64_t> queryDims{queryType.getSizes()};
        int64_t batchSize = queryDims[0];
        int64_t sequenceLength = queryDims[1];
        int64_t hiddenSize = queryDims[2];
        int64_t headSize = hiddenSize / numHeads;

        Value cstBatchSize = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getI64IntegerAttr(batchSize));
        Value cstSequenceLength = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getI64IntegerAttr(sequenceLength));
        Value cstHiddenSize = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getI64IntegerAttr(hiddenSize));
        Value cstHeadSize = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getI64IntegerAttr(headSize));
        Value cstNumHeads = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getI64IntegerAttr(numHeads));
        Value cstKVNumHeads = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getI64IntegerAttr(kvNumHeads));

        // Reshape Query, Key and Value as follows:
        // Query: (batch_size, sequence_length, hidden_size)
        //     -> (batch_size, num_heads, sequence_length, head_size)
        // Key: (batch_size, kv_sequence_length, kv_hidden_size)
        //   -> (batch_size, kv_num_heads, sequence_length, head_size)
        // Value: (batch_size, kv_sequence_length, kv_hidden_size)
        //     -> (batch_size, kv_num_heads, sequence_length, head_size)

        // Reshaping query.
        SmallVector<int64_t> queryReshapeSizesInt{batchSize, numHeads,
                                                  sequenceLength, headSize};
        Value queryReshapeSizesList =
            rewriter.create<Torch::PrimListConstructOp>(
                binder.getLoc(),
                Torch::ListType::get(Torch::IntType::get(query.getContext())),
                llvm::SmallVector<Value>{cstBatchSize, cstNumHeads,
                                         cstSequenceLength, cstHeadSize});
        Value qInput = rewriter.create<Torch::AtenReshapeOp>(
            loc,
            queryType.getWithSizesAndDtype(queryReshapeSizesInt,
                                           queryType.getOptionalDtype()),
            query, queryReshapeSizesList);

        // Reshaping key.
        SmallVector<int64_t> kvReshapeSizesInt{batchSize, kvNumHeads,
                                               sequenceLength, headSize};
        Value kvReshapeSizesList = rewriter.create<Torch::PrimListConstructOp>(
            binder.getLoc(),
            Torch::ListType::get(Torch::IntType::get(query.getContext())),
            llvm::SmallVector<Value>{cstBatchSize, cstKVNumHeads,
                                     cstSequenceLength, cstHeadSize});
        Torch::ValueTensorType keyType =
            cast<Torch::ValueTensorType>(key.getType());
        Value kInput = rewriter.create<Torch::AtenReshapeOp>(
            loc,
            keyType.getWithSizesAndDtype(kvReshapeSizesInt,
                                         keyType.getOptionalDtype()),
            key, kvReshapeSizesList);

        // Reshaping value.
        Torch::ValueTensorType valueType =
            cast<Torch::ValueTensorType>(value.getType());
        Value vInput = rewriter.create<Torch::AtenReshapeOp>(
            loc,
            valueType.getWithSizesAndDtype(kvReshapeSizesInt,
                                           valueType.getOptionalDtype()),
            value, kvReshapeSizesList);

        Value cstNone = rewriter.create<Torch::ConstantNoneOp>(loc);
        Value cstFalse = rewriter.create<Torch::ConstantBoolOp>(loc, false);

        Value qRotary = qInput, kRotary = kInput;
        if (doRotary) {
          // `totalSequenceLength` is a scalar tensor.
          Value scalarTotalSeqLens = rewriter.create<Torch::AtenItemOp>(
              loc, rewriter.getType<Torch::IntType>(), totalSequenceLength);
          Value cstIntOne = rewriter.create<Torch::ConstantIntOp>(
              binder.getLoc(), rewriter.getI64IntegerAttr(1));
          Type boolTy = rewriter.getType<Torch::BoolType>();
          Value condA = rewriter.create<Torch::AtenGtIntOp>(
              loc, boolTy, cstSequenceLength, cstIntOne);
          Value condB = rewriter.create<Torch::AtenNeIntOp>(
              loc, boolTy, cstSequenceLength, scalarTotalSeqLens);
          //   if (sequence_length > 1 && sequence_length !=
          //   total_sequence_length)
          //         is_subsequent_prompt = false;  // Subsequent prompt
          Value isSubsequentPrompt = rewriter.create<Torch::Aten__And__BoolOp>(
              loc, boolTy, condA, condB);

          // Generating position_ids for rotary_embedding as follows:
          //   pos_ids_a = torch.zeros((batch_size, seq_len), dtype=torch.int64)
          //
          //   total_seqlens = seqlens_k + 1
          //   past_seqlens = total_seqlens - sequence_length
          //   pos_ids = torch.arange(sequence_length,
          //             dtype=torch.int64).repeat(batch_size, 1)
          //   pos_ids = pos_ids + past_seqlens.view(-1, 1)
          //   cond = pos_ids < total_seqlens.view(-1, 1)
          //   one_tensor = torch.tensor(1, dtype=torch.int64)
          //   pos_ids_b = torch.where(cond, pos_ids, one_tensor)
          //
          //  if subsequent_prompt:
          //      pos_ids = pos_ids_b
          //  else:
          //      pos_ids = pos_ids_a
          SmallVector<int64_t> positionIdsSizeInt{batchSize, sequenceLength};
          Torch::ValueTensorType positionIdsType = Torch::ValueTensorType::get(
              context, positionIdsSizeInt,
              IntegerType::get(context, 64, IntegerType::Signed));
          Value cstInt64Dtype = rewriter.create<Torch::ConstantIntOp>(
              binder.getLoc(), rewriter.getI64IntegerAttr(
                                   (int)torch_upstream::ScalarType::Long));

          Value cstInterleaved = rewriter.create<Torch::ConstantIntOp>(
              binder.getLoc(), rewriter.getI64IntegerAttr(rotaryInterleaved));
          Value cstIntZero = rewriter.create<Torch::ConstantIntOp>(
              binder.getLoc(), rewriter.getI64IntegerAttr(0));
          Value cstFloatOne = rewriter.create<Torch::ConstantFloatOp>(
              binder.getLoc(), rewriter.getType<Torch::FloatType>(),
              rewriter.getF64FloatAttr(1.0));

          Value positionIdsA, positionIdsB;

          Value posIdsSizeList = rewriter.create<Torch::PrimListConstructOp>(
              loc,
              rewriter.getType<Torch::ListType>(
                  rewriter.getType<Torch::IntType>()),
              SmallVector<Value>{cstBatchSize, cstSequenceLength});
          positionIdsA = rewriter.create<Torch::AtenZerosOp>(
              loc, positionIdsType, /*size=*/posIdsSizeList,
              /*dtype=*/cstInt64Dtype,
              /*layout=*/cstNone, /*device=*/cstNone,
              /*pin_memory=*/cstNone);

          // Convert seqlens_k which is a tensor of type si32 to si64.
          Torch::ValueTensorType seqLensKType =
              cast<Torch::ValueTensorType>(seqlensK.getType());
          seqlensK = rewriter.create<Torch::AtenToDtypeOp>(
              loc,
              seqLensKType.getWithSizesAndDtype(
                  std::nullopt,
                  rewriter.getIntegerType(/*width=*/64, /*isSigned=*/true)),
              seqlensK, cstInt64Dtype, /*non_blocking=*/cstFalse,
              /*copy=*/cstFalse, /*memory_format=*/cstNone);
          Value totalSeqLens = rewriter.create<Torch::AtenAddScalarOp>(
              loc, seqlensK.getType(), /*self=*/seqlensK, /*other=*/cstIntOne,
              /*alpha=*/cstIntOne);
          Value pastSeqLens = rewriter.create<Torch::AtenSubScalarOp>(
              loc, totalSeqLens.getType(), /*self=*/totalSeqLens,
              /*other=*/cstSequenceLength, /*alpha=*/cstIntOne);
          Torch::ValueTensorType initPosIdsType = Torch::ValueTensorType::get(
              context, {sequenceLength},
              IntegerType::get(context, 64, IntegerType::Signed));
          Value initPosIds = rewriter.create<Torch::AtenArangeOp>(
              loc, initPosIdsType, cstSequenceLength, cstInt64Dtype,
              /*layout=*/cstNone,
              /*device=*/cstNone, /*pin_memory=*/cstNone);
          Value repeatValuesList = rewriter.create<Torch::PrimListConstructOp>(
              binder.getLoc(),
              Torch::ListType::get(Torch::IntType::get(context)),
              llvm::SmallVector<Value>{cstBatchSize, cstIntOne});
          positionIdsB = rewriter.create<Torch::AtenRepeatOp>(
              loc, positionIdsType, initPosIds, /*repeats=*/repeatValuesList);

          Value cstIntMinusOne = rewriter.create<Torch::ConstantIntOp>(
              binder.getLoc(), rewriter.getI64IntegerAttr(1));
          Value viewSizeList = rewriter.create<Torch::PrimListConstructOp>(
              binder.getLoc(),
              Torch::ListType::get(Torch::IntType::get(context)),
              llvm::SmallVector<Value>{cstIntMinusOne, cstIntOne});

          Torch::ValueTensorType seqLensViewType = Torch::ValueTensorType::get(
              context, llvm::SmallVector<int64_t>{batchSize, 1},
              IntegerType::get(context, 64, IntegerType::Signed));
          pastSeqLens = rewriter.create<Torch::AtenViewOp>(
              loc, seqLensViewType, pastSeqLens, viewSizeList);

          positionIdsB = rewriter.create<Torch::AtenAddTensorOp>(
              loc, positionIdsType, positionIdsB, pastSeqLens,
              /*alpha=*/cstIntOne);

          totalSeqLens = rewriter.create<Torch::AtenViewOp>(
              loc, seqLensViewType, totalSeqLens, viewSizeList);
          Value cond = rewriter.create<Torch::AtenLtTensorOp>(
              loc,
              positionIdsType.getWithSizesAndDtype(positionIdsType.getSizes(),
                                                   rewriter.getI1Type()),
              positionIdsB, totalSeqLens);

          Value cstOneTensorDataList =
              rewriter.create<Torch::PrimListConstructOp>(
                  loc,
                  rewriter.getType<Torch::ListType>(
                      rewriter.getType<Torch::IntType>()),
                  SmallVector<Value>{cstIntOne});
          Value cstOneTensor = rewriter.create<Torch::AtenTensorOp>(
              loc,
              Torch::ValueTensorType::get(
                  context, {},
                  IntegerType::get(context, 64, IntegerType::Signed)),
              cstOneTensorDataList, /*dtype=*/cstInt64Dtype,
              /*layout=*/cstNone, /*requires_grad=*/cstFalse);

          positionIdsB = rewriter.create<Torch::AtenWhereSelfOp>(
              loc, positionIdsType, cond, positionIdsB, cstOneTensor);

          isSubsequentPrompt = rewriter.create<Torch::AtenIntBoolOp>(
              loc, rewriter.getType<Torch::IntType>(), isSubsequentPrompt);
          isSubsequentPrompt = rewriter.create<Torch::AtenFullOp>(
              loc,
              Torch::ValueTensorType::get(context, positionIdsSizeInt,
                                          rewriter.getI1Type()),
              /*size=*/posIdsSizeList, /*fill_value=*/isSubsequentPrompt,
              /*dtype=*/
              rewriter.create<Torch::ConstantIntOp>(
                  binder.getLoc(), rewriter.getI64IntegerAttr(
                                       (int)torch_upstream::ScalarType::Bool)),
              /*layout=*/cstNone, /*device=*/cstNone, /*pin_memory=*/cstNone);
          Value positionIds = rewriter.create<Torch::AtenWhereSelfOp>(
              loc, positionIdsType, isSubsequentPrompt, positionIdsB,
              positionIdsA);

          // Performing RotaryEmbedding over Query and Key.
          qRotary = rewriter.create<Torch::OnnxVariantRotaryEmbeddingOp>(
              loc, qInput.getType(), qInput, positionIds, cosCache, sinCache,
              cstInterleaved, /*is_packed_batching=*/cstIntZero,
              /*num_heads=*/cstIntZero, /*rotary_embedding_dim=*/cstIntZero,
              /*scale=*/cstFloatOne);

          kRotary = rewriter.create<Torch::OnnxVariantRotaryEmbeddingOp>(
              loc, qInput.getType(), kInput, positionIds, cosCache, sinCache,
              cstInterleaved, /*is_packed_batching=*/cstIntZero,
              /*num_heads=*/cstIntZero, /*rotary_embedding_dim=*/cstIntZero,
              /*scale=*/cstFloatOne);
        }

        // Do attention.
        Value cstEnableGQA = rewriter.create<Torch::ConstantBoolOp>(loc, true);
        Value cstFloatZero = rewriter.create<Torch::ConstantFloatOp>(
            binder.getLoc(), rewriter.getType<Torch::FloatType>(),
            rewriter.getF64FloatAttr(0.0));
        Value cstScale = cstNone;
        if (scale != 0.0f)
          cstScale = rewriter.create<Torch::ConstantFloatOp>(
              binder.getLoc(), rewriter.getType<Torch::FloatType>(),
              rewriter.getF64FloatAttr(scale));
        Value attention =
            rewriter.create<Torch::AtenScaledDotProductAttentionOp>(
                loc, qRotary.getType(), qRotary, kRotary, vInput,
                /*attn_mask=*/cstNone,
                /*dropout_p=*/cstFloatZero, /*is_causal=*/cstFalse, cstScale,
                cstEnableGQA);
        // Reshaping the attention result from:
        //    (batch_size, num_heads, sequence_length, head_size)
        // -> (batch_size, sequence_length, hidden_size)
        Value attentionResultSizesList =
            rewriter.create<Torch::PrimListConstructOp>(
                binder.getLoc(),
                Torch::ListType::get(
                    Torch::IntType::get(attention.getContext())),
                llvm::SmallVector<Value>{cstBatchSize, cstSequenceLength,
                                         cstHiddenSize});
        attention = rewriter.create<Torch::AtenReshapeOp>(
            loc, resultTypes[0], attention, attentionResultSizesList);

        // Compute 2nd and 3rd result: present_key, present_value.
        // present_key = torch.cat([past_key, key], dim=2) or past_key
        // present_value = torch.cat([past_value, value], dim=2) or past_value
        Value presentKey = pastKey, presentValue = pastValue;
        if (!llvm::equal(
                cast<Torch::ValueTensorType>(pastKey.getType()).getSizes(),
                cast<Torch::ValueTensorType>(resultTypes[1]).getSizes())) {
          Value cstConcatDim = rewriter.create<Torch::ConstantIntOp>(
              binder.getLoc(), rewriter.getI64IntegerAttr(2));
          Type kvListElemType = keyType.getWithSizesAndDtype(
              /*optionalSizes=*/std::nullopt,
              /*optionalDtype=*/nullptr);
          Type kvListType = Torch::ListType::get(kvListElemType);
          Value keyList = rewriter.create<Torch::PrimListConstructOp>(
              loc, kvListType, SmallVector<Value>{pastKey, kRotary});
          presentKey = rewriter.create<Torch::AtenCatOp>(loc, resultTypes[1],
                                                         keyList, cstConcatDim);
        }

        if (!llvm::equal(
                cast<Torch::ValueTensorType>(pastValue.getType()).getSizes(),
                cast<Torch::ValueTensorType>(resultTypes[2]).getSizes())) {
          Value cstConcatDim = rewriter.create<Torch::ConstantIntOp>(
              binder.getLoc(), rewriter.getI64IntegerAttr(2));
          Type kvListElemType = keyType.getWithSizesAndDtype(
              /*optionalSizes=*/std::nullopt,
              /*optionalDtype=*/nullptr);
          Type kvListType = Torch::ListType::get(kvListElemType);
          Value valueList = rewriter.create<Torch::PrimListConstructOp>(
              loc, kvListType, SmallVector<Value>{pastValue, vInput});
          presentValue = rewriter.create<Torch::AtenCatOp>(
              loc, resultTypes[2], valueList, cstConcatDim);
        }

        rewriter.replaceOp(binder.op, {attention, presentKey, presentValue});
        return success();
      });
  patterns.onOp(
      "QLinearAdd", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        Location loc = binder.getLoc();
        Torch::ValueTensorType resultType;
        llvm::SmallVector<Value> operands;
        if (binder.tensorOperandsList(operands) ||
            binder.tensorResultType(resultType))
          return failure();

        if (operands.size() != 8)
          return rewriter.notifyMatchFailure(
              binder.op, "Unimplemented: expected 8 input operands");

        Value a, aScale, aZp, b, bScale, bZp, cScale, cZp;

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[1],
                /*zero_point=*/operands[2], aScale, aZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[4],
                /*zero_point=*/operands[5], bScale, bZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[6],
                /*zero_point=*/operands[7], cScale, cZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(createDequantizeTensor(rewriter, loc, /*input=*/operands[0],
                                          /*scale=*/aScale, /*zero_point=*/aZp,
                                          /*output=*/a)))
          return rewriter.notifyMatchFailure(
              binder.op, "Failed to dequantize the input tensor `a` because of "
                         "missing sizes");

        if (failed(createDequantizeTensor(rewriter, loc, /*input=*/operands[3],
                                          /*scale=*/bScale, /*zero_point=*/bZp,
                                          /*output=*/b)))
          return rewriter.notifyMatchFailure(
              binder.op, "Failed to dequantize the input tensor `b` because of "
                         "missing sizes");

        // Computing the result of "Add".
        auto cTy = rewriter.getType<Torch::ValueTensorType>(
            resultType.getOptionalSizes(), rewriter.getF32Type());
        Value alpha = rewriter.create<Torch::ConstantFloatOp>(
            loc, rewriter.getF64FloatAttr(1.0));
        Value c = rewriter.create<Torch::AtenAddTensorOp>(binder.getLoc(), cTy,
                                                          a, b, alpha);

        // Quantizing the result of "Add" operation.
        cTy = dyn_cast<Torch::ValueTensorType>(
            getQTorchTypeFromTorchIntType(resultType));
        Value dtyVal = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getType<Torch::IntType>(),
            rewriter.getIntegerAttr(
                rewriter.getIntegerType(64),
                static_cast<int64_t>(
                    Torch::getScalarTypeForType(cTy.getDtype()))));
        c = rewriter.create<Torch::AtenQuantizePerTensorOp>(
            binder.getLoc(), cTy, c, cScale, cZp, dtyVal);
        rewriter.replaceOpWithNewOp<Torch::AtenIntReprOp>(binder.op, resultType,
                                                          c);
        return success();
      });
  patterns.onOp(
      "QLinearLeakyRelu", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        Location loc = binder.getLoc();
        Torch::ValueTensorType resultType;
        llvm::SmallVector<Value> operands;
        float alpha;
        if (binder.tensorOperandsList(operands) ||
            binder.tensorResultType(resultType) ||
            binder.f32FloatAttr(alpha, "alpha"))
          return failure();

        if (operands.size() != 5)
          return rewriter.notifyMatchFailure(
              binder.op, "Unimplemented: expected 5 input operands");

        Value x, xScale, xZp, yScale, yZp;

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[1],
                /*zero_point=*/operands[2], xScale, xZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[3],
                /*zero_point=*/operands[4], yScale, yZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(createDequantizeTensor(rewriter, loc, /*input=*/operands[0],
                                          /*scale=*/xScale, /*zero_point=*/xZp,
                                          /*output=*/x)))
          return rewriter.notifyMatchFailure(
              binder.op, "Failed to dequantize the input tensor `x` because of "
                         "missing sizes");

        // Computing the LeakyRelu result.
        Value constAlpha = rewriter.create<Torch::ConstantFloatOp>(
            loc, rewriter.getType<Torch::FloatType>(),
            rewriter.getF64FloatAttr((double)alpha));
        auto yTy = rewriter.getType<Torch::ValueTensorType>(
            resultType.getOptionalSizes(), rewriter.getF32Type());
        Value y =
            rewriter.create<Torch::AtenLeakyReluOp>(loc, yTy, x, constAlpha);

        // Quantizing the result of LeakyRelu op.
        yTy = dyn_cast<Torch::ValueTensorType>(
            getQTorchTypeFromTorchIntType(resultType));
        Value dtyVal = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getType<Torch::IntType>(),
            rewriter.getIntegerAttr(
                rewriter.getIntegerType(64),
                static_cast<int64_t>(
                    Torch::getScalarTypeForType(yTy.getDtype()))));
        y = rewriter.create<Torch::AtenQuantizePerTensorOp>(loc, yTy, y, yScale,
                                                            yZp, dtyVal);
        rewriter.replaceOpWithNewOp<Torch::AtenIntReprOp>(binder.op, resultType,
                                                          y);
        return success();
      });
  patterns.onOp(
      "QLinearConcat", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        Location loc = binder.getLoc();
        Torch::ValueTensorType resultType;
        SmallVector<Value> operands;
        int64_t axis;
        if (binder.tensorOperandsList(operands) ||
            binder.s64IntegerAttr(axis, "axis") ||
            binder.tensorResultType(resultType))
          return failure();

        SmallVector<Value> inputs, inputScales, inputZeroPoints;
        for (unsigned i = 2; i < operands.size(); i = i + 3) {
          inputs.push_back(operands[i]);
          inputScales.push_back(operands[i + 1]);
          inputZeroPoints.push_back(operands[i + 2]);
        }

        unsigned numInputs = (operands.size() - 2) / 3;
        if (!(llvm::all_equal({inputs.size(), inputScales.size(),
                               inputZeroPoints.size()}) &&
              inputs.size() == numInputs))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible number of input operands, scales and/or "
                         "zero-points");

        // Preparing the dequantized inputs.
        SmallVector<Value> dequantizedInputs;
        for (unsigned i = 0; i < numInputs; i++) {
          Value scale, zeroPoint;
          if (failed(extractPerTensorQuantizationArguments(
                  rewriter, loc, /*scale=*/inputScales[i],
                  /*zero_point=*/inputZeroPoints[i], scale, zeroPoint)))
            return rewriter.notifyMatchFailure(
                binder.op, "Incompatible scale and zero-points argument for "
                           "per-tensor quantization");

          Value dequantizedInput;
          if (failed(createDequantizeTensor(rewriter, loc, inputs[i], scale,
                                            zeroPoint,
                                            /*output=*/dequantizedInput)))
            return rewriter.notifyMatchFailure(
                binder.op, "Failed to dequantize the input tensor because of "
                           "missing sizes");

          dequantizedInputs.push_back(dequantizedInput);
        }

        // Concatenating the inputs.
        Type listElemType =
            cast<Torch::BaseTensorType>(dequantizedInputs[0].getType())
                .getWithSizesAndDtype(/*optionalSizes=*/std::nullopt,
                                      /*optionalDtype=*/nullptr);
        Type listType = Torch::ListType::get(listElemType);
        Value tensorList = rewriter.create<Torch::PrimListConstructOp>(
            binder.op->getLoc(), listType, dequantizedInputs);
        Value cstAxis = rewriter.create<Torch::ConstantIntOp>(
            loc, rewriter.getI64IntegerAttr(axis));
        auto concatTy = rewriter.getType<Torch::ValueTensorType>(
            resultType.getOptionalSizes(), rewriter.getF32Type());
        Value concat = rewriter.create<Torch::AtenCatOp>(loc, concatTy,
                                                         tensorList, cstAxis);

        // Quantizing the result of concatenated inputs.
        Value yScale, yZp;
        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[0],
                /*zero_point=*/operands[1], yScale, yZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible scale and zero-points argument for "
                         "per-tensor quantization");
        Torch::ValueTensorType yTy = dyn_cast<Torch::ValueTensorType>(
            getQTorchTypeFromTorchIntType(resultType));
        Value dtyVal = rewriter.create<Torch::ConstantIntOp>(
            loc, rewriter.getType<Torch::IntType>(),
            rewriter.getIntegerAttr(
                rewriter.getIntegerType(64),
                static_cast<int64_t>(
                    Torch::getScalarTypeForType(yTy.getDtype()))));
        Value result = rewriter.create<Torch::AtenQuantizePerTensorOp>(
            loc, yTy, concat, yScale, yZp, dtyVal);
        rewriter.replaceOpWithNewOp<Torch::AtenIntReprOp>(binder.op, resultType,
                                                          result);
        return success();
      });
  patterns.onOp(
      "QLinearGlobalAveragePool", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        Location loc = binder.getLoc();
        Torch::ValueTensorType resultType;
        llvm::SmallVector<Value> operands;
        int64_t channelsLast;
        if (binder.tensorOperands(operands, 5) ||
            binder.tensorResultType(resultType) ||
            binder.s64IntegerAttr(channelsLast, "channels_last"))
          return failure();

        // TODO: Add support for channels_last attribute.
        if (channelsLast)
          return rewriter.notifyMatchFailure(
              binder.op,
              "Unimplemented: support not present for channels_last attribute");

        auto xTy = dyn_cast<Torch::ValueTensorType>(operands[0].getType());
        if (!xTy || !xTy.hasSizes())
          return rewriter.notifyMatchFailure(
              binder.op, "Expected input argument `x` to have sizes");
        ArrayRef<int64_t> inputShape = xTy.getSizes();

        if (!resultType || !resultType.hasSizes()) {
          return rewriter.notifyMatchFailure(
              binder.op, "Expected result type having sizes");
        }
        ArrayRef<int64_t> resultShape = resultType.getSizes();

        Value x, xScale, xZp, yScale, yZp;

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[1],
                /*zero_point=*/operands[2], xScale, xZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[3],
                /*zero_point=*/operands[4], yScale, yZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(createDequantizeTensor(rewriter, loc, /*input=*/operands[0],
                                          /*scale=*/xScale, /*zero_point=*/xZp,
                                          /*output=*/x)))
          return rewriter.notifyMatchFailure(
              binder.op, "Failed to dequantize the input tensor `x` because of "
                         "missing sizes");

        // Computing the AvgPool result.
        SmallVector<Value> cstKernel, cstPadding, cstStrides;
        Value cstZero = rewriter.create<Torch::ConstantIntOp>(
            loc, rewriter.getI64IntegerAttr(0));
        Value cstOne = rewriter.create<Torch::ConstantIntOp>(
            loc, rewriter.getI64IntegerAttr(1));
        unsigned inputRank = inputShape.size();
        for (unsigned i = 2; i < inputRank; i++) {
          if (inputShape[i] == Torch::kUnknownSize) {
            Value dim = rewriter.create<Torch::ConstantIntOp>(
                loc, rewriter.getI64IntegerAttr(i));
            Value inputDimSize =
                rewriter.create<Torch::AtenSizeIntOp>(loc, x, dim);
            cstKernel.push_back(inputDimSize);
          } else {
            int64_t kernelSize = inputShape[i] - resultShape[i] + 1;
            cstKernel.push_back(rewriter.create<Torch::ConstantIntOp>(
                loc, rewriter.getI64IntegerAttr(kernelSize)));
          }
          cstPadding.push_back(cstZero);
          cstStrides.push_back(cstOne);
        }
        Value kernelSizeList = rewriter.create<Torch::PrimListConstructOp>(
            loc,
            Torch::ListType::get(Torch::IntType::get(binder.op->getContext())),
            cstKernel);
        Value paddingList = rewriter.create<Torch::PrimListConstructOp>(
            loc,
            Torch::ListType::get(Torch::IntType::get(binder.op->getContext())),
            cstPadding);
        Value stridesList = rewriter.create<Torch::PrimListConstructOp>(
            loc,
            Torch::ListType::get(Torch::IntType::get(binder.op->getContext())),
            cstStrides);
        Value cstFalse = rewriter.create<Torch::ConstantBoolOp>(loc, false);
        Value cstCeilMode = cstFalse;
        Value cstCountIncludePad = cstFalse;
        Value cstNone = rewriter.create<Torch::ConstantNoneOp>(loc);

        auto yTy = rewriter.getType<Torch::ValueTensorType>(
            resultShape, rewriter.getF32Type());
        Value avgpool;
        if (inputRank == 3) {
          avgpool = rewriter.create<Torch::AtenAvgPool1dOp>(
              loc, yTy, x, kernelSizeList, stridesList, paddingList,
              cstCeilMode, cstCountIncludePad);
        } else if (inputRank == 4) {
          avgpool = rewriter.create<Torch::AtenAvgPool2dOp>(
              loc, yTy, x, kernelSizeList, stridesList, paddingList,
              cstCeilMode, cstCountIncludePad,
              /*divisor_override=*/cstNone);
        } else if (inputRank == 5) {
          avgpool = rewriter.create<Torch::AtenAvgPool3dOp>(
              loc, yTy, x, kernelSizeList, stridesList, paddingList,
              cstCeilMode, cstCountIncludePad,
              /*divisor_override=*/cstNone);
        } else {
          return failure();
        }

        // Quantizing the result of AvgPool op.
        yTy = dyn_cast<Torch::ValueTensorType>(
            getQTorchTypeFromTorchIntType(resultType));
        Value dtyVal = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getType<Torch::IntType>(),
            rewriter.getIntegerAttr(
                rewriter.getIntegerType(64),
                static_cast<int64_t>(
                    Torch::getScalarTypeForType(yTy.getDtype()))));
        avgpool = rewriter.create<Torch::AtenQuantizePerTensorOp>(
            loc, yTy, avgpool, yScale, yZp, dtyVal);
        rewriter.replaceOpWithNewOp<Torch::AtenIntReprOp>(binder.op, resultType,
                                                          avgpool);
        return success();
      });
  patterns.onOp(
      "QLinearSigmoid", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        Location loc = binder.getLoc();
        Torch::ValueTensorType resultType;
        llvm::SmallVector<Value> operands;
        if (binder.tensorOperandsList(operands) ||
            binder.tensorResultType(resultType))
          return failure();

        if (operands.size() != 5)
          return rewriter.notifyMatchFailure(
              binder.op, "Unimplemented: expected 5 input operands");

        Value x, xScale, xZp, yScale, yZp;

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[1],
                /*zero_point=*/operands[2], xScale, xZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[3],
                /*zero_point=*/operands[4], yScale, yZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(createDequantizeTensor(rewriter, loc, /*input=*/operands[0],
                                          /*scale=*/xScale, /*zero_point=*/xZp,
                                          /*output=*/x)))
          return rewriter.notifyMatchFailure(
              binder.op, "Failed to dequantize the input tensor `x` because of "
                         "missing sizes");

        // Computing the Sigmoid result.
        auto yTy = rewriter.getType<Torch::ValueTensorType>(
            resultType.getOptionalSizes(), rewriter.getF32Type());
        Value y = rewriter.create<Torch::AtenSigmoidOp>(loc, yTy, x);

        // Quantizing the result of Sigmoid op.
        yTy = dyn_cast<Torch::ValueTensorType>(
            getQTorchTypeFromTorchIntType(resultType));
        Value dtyVal = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getType<Torch::IntType>(),
            rewriter.getIntegerAttr(
                rewriter.getIntegerType(64),
                static_cast<int64_t>(
                    Torch::getScalarTypeForType(yTy.getDtype()))));
        y = rewriter.create<Torch::AtenQuantizePerTensorOp>(loc, yTy, y, yScale,
                                                            yZp, dtyVal);
        rewriter.replaceOpWithNewOp<Torch::AtenIntReprOp>(binder.op, resultType,
                                                          y);
        return success();
      });
  patterns.onOp(
      "QLinearAveragePool", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        Location loc = binder.getLoc();
        Torch::ValueTensorType resultType;
        llvm::SmallVector<Value> operands;
        int64_t channelsLast;
        if (binder.tensorOperandsList(operands) ||
            binder.tensorResultType(resultType) ||
            binder.s64IntegerAttr(channelsLast, "channels_last"))
          return failure();

        // TODO: Add support for channels_last attribute.
        if (channelsLast)
          return rewriter.notifyMatchFailure(
              binder.op,
              "Unimplemented: support not present for channels_last attribute");

        if (operands.size() != 5)
          return rewriter.notifyMatchFailure(
              binder.op, "Unimplemented: expected 5 input operands");

        Value x, xScale, xZp, yScale, yZp;

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[1],
                /*zero_point=*/operands[2], xScale, xZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[3],
                /*zero_point=*/operands[4], yScale, yZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(createDequantizeTensor(rewriter, loc, /*input=*/operands[0],
                                          /*scale=*/xScale, /*zero_point=*/xZp,
                                          /*output=*/x)))
          return rewriter.notifyMatchFailure(
              binder.op, "Failed to dequantize the input tensor `x` because of "
                         "missing sizes");

        // Creating Onnx.AveragePool op.
        llvm::SmallVector<Value> newOperands = {x};
        llvm::SmallVector<NamedAttribute> newAttributes;
        newAttributes.push_back(rewriter.getNamedAttr(
            "name", rewriter.getStringAttr("onnx.AveragePool")));
        for (auto namedAttr : binder.op->getAttrDictionary()) {
          if (namedAttr.getName().getValue().compare("name") == 0)
            continue;
          newAttributes.push_back(namedAttr);
        }

        auto yTy = rewriter.getType<Torch::ValueTensorType>(
            resultType.getOptionalSizes(), rewriter.getF32Type());
        Value averagePool =
            rewriter
                .create<Torch::OperatorOp>(binder.getLoc(), yTy, newOperands,
                                           newAttributes,
                                           binder.op->getRegions().size())
                .getResult(0);

        // Quantizing the result of AveragePool op.
        yTy = dyn_cast<Torch::ValueTensorType>(
            getQTorchTypeFromTorchIntType(resultType));
        Value dtyVal = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getType<Torch::IntType>(),
            rewriter.getIntegerAttr(
                rewriter.getIntegerType(64),
                static_cast<int64_t>(
                    Torch::getScalarTypeForType(yTy.getDtype()))));
        averagePool = rewriter.create<Torch::AtenQuantizePerTensorOp>(
            loc, yTy, averagePool, yScale, yZp, dtyVal);
        rewriter.replaceOpWithNewOp<Torch::AtenIntReprOp>(binder.op, resultType,
                                                          averagePool);
        return success();
      });
  patterns.onOp(
      "FusedMatMul", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        Torch::ValueTensorType resultType;
        Value lhs, rhs;
        int64_t transA, transB, transBatchA, transBatchB;
        if (binder.tensorOperands(lhs, rhs) ||
            binder.s64IntegerAttr(transA, "transA", 0) ||
            binder.s64IntegerAttr(transB, "transB", 0) ||
            binder.s64IntegerAttr(transBatchA, "transBatchA", 0) ||
            binder.s64IntegerAttr(transBatchB, "transBatchB", 0) ||
            binder.tensorResultType(resultType))
          return failure();

        // Transposing the LHS argument.
        Value transposedLhs = lhs;
        if (transA) {
          // Determine the rank of lhs tensor.
          std::optional<unsigned> maybeRank = Torch::getTensorRank(lhs);
          if (!maybeRank)
            return rewriter.notifyMatchFailure(
                binder.op, "Unimplemented: unranked lhs tensor");
          unsigned lhsRank = *maybeRank;
          if (failed(createTorchTransposeOp(
                  rewriter, binder.getLoc(), lhs,
                  /*dimA=*/lhsRank - 2, /*dimB=*/lhsRank - 1, transposedLhs)))
            return rewriter.notifyMatchFailure(
                binder.op, "Failed to create TorchTranspose op for lhs");
        }

        // Transposing the RHS argument.
        Value transposedRhs = rhs;
        if (transB) {
          std::optional<unsigned> maybeRank = Torch::getTensorRank(rhs);
          if (!maybeRank)
            return rewriter.notifyMatchFailure(
                binder.op, "Unimplemented: unranked rhs tensor");
          unsigned rhsRank = *maybeRank;
          if (failed(createTorchTransposeOp(
                  rewriter, binder.getLoc(), rhs,
                  /*dimA=*/rhsRank - 2, /*dimB=*/rhsRank - 1, transposedRhs)))
            return rewriter.notifyMatchFailure(
                binder.op, "Failed to create TorchTranspose op for rhs");
        }

        // TODO: Add support for `transBatchA` and `transBatchB`
        // attribute.
        if (transBatchA || transBatchB)
          return rewriter.notifyMatchFailure(
              binder.op, "Unimplemented: support not present for "
                         "transBatchA and transBatchB attribute");

        rewriter.replaceOpWithNewOp<Torch::AtenMatmulOp>(
            binder.op, resultType, transposedLhs, transposedRhs);
        return success();
      });
  patterns.onOp(
      "QLinearMul", 1,
      [](OpBinder binder, ConversionPatternRewriter &rewriter) {
        Location loc = binder.getLoc();
        Torch::ValueTensorType resultType;
        llvm::SmallVector<Value> operands;
        if (binder.tensorOperandsList(operands) ||
            binder.tensorResultType(resultType))
          return failure();

        if (operands.size() != 8)
          return rewriter.notifyMatchFailure(
              binder.op, "Unimplemented: expected 8 input operands");

        Value a, b, aScale, aZp, bScale, bZp, cScale, cZp;

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[1],
                /*zero_point=*/operands[2], aScale, aZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[4],
                /*zero_point=*/operands[5], bScale, bZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(extractPerTensorQuantizationArguments(
                rewriter, loc, /*scale=*/operands[6],
                /*zero_point=*/operands[7], cScale, cZp)))
          return rewriter.notifyMatchFailure(
              binder.op, "Incompatible arguments for per-tensor quantization");

        if (failed(createDequantizeTensor(rewriter, loc, /*input=*/operands[0],
                                          /*scale=*/aScale, /*zero_point=*/aZp,
                                          /*output=*/a)))
          return rewriter.notifyMatchFailure(
              binder.op, "Failed to dequantize the input tensor `a` because of "
                         "missing sizes");

        if (failed(createDequantizeTensor(rewriter, loc, /*input=*/operands[3],
                                          /*scale=*/bScale, /*zero_point=*/bZp,
                                          /*output=*/b)))
          return rewriter.notifyMatchFailure(
              binder.op, "Failed to dequantize the input tensor `b` because of "
                         "missing sizes");

        // Computing the Mul result.
        auto cTy = rewriter.getType<Torch::ValueTensorType>(
            resultType.getOptionalSizes(), rewriter.getF32Type());
        Value c =
            rewriter.create<Torch::AtenMulTensorOp>(binder.getLoc(), cTy, a, b);

        // Quantizing the result of Mul operation.
        cTy = dyn_cast<Torch::ValueTensorType>(
            getQTorchTypeFromTorchIntType(resultType));
        Value dtyVal = rewriter.create<Torch::ConstantIntOp>(
            binder.getLoc(), rewriter.getType<Torch::IntType>(),
            rewriter.getIntegerAttr(
                rewriter.getIntegerType(64),
                static_cast<int64_t>(
                    Torch::getScalarTypeForType(cTy.getDtype()))));
        c = rewriter.create<Torch::AtenQuantizePerTensorOp>(
            binder.getLoc(), cTy, c, cScale, cZp, dtyVal);
        rewriter.replaceOpWithNewOp<Torch::AtenIntReprOp>(binder.op, resultType,
                                                          c);
        return success();
      });
}
