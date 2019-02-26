/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file test_op.h
 * \brief operator unit test utility functions
 * \author Chris Olivier
 *
 * These classes offer a framework for developing, testing and debugging operators
 * in C++.  They work for both CPU and GPU modes, as well as offer a timing
 * infrastructure in order to test inidividual operator performance.
 *
 * Operator data can be validated against general logic,
 * stored scalar values (which can be generated by this code from an existing operator via
 * BasicOperatorData::dumpC(), as well as against each other (ie check that
 * GPU, CPU, MKL, and CUDNN operators produce the same output given the same input.
 *
 * test_util.h: General testing utility functionality
 * test_perf.h: Performance-related classes
 * test_op.h:   Operator-specific testing classes
 */
#ifndef TEST_LEGACY_OP_H_
#define TEST_LEGACY_OP_H_

#include <list>
#include <string>
#include <algorithm>
#include <map>
#include <vector>
#include <list>
#include "profiler/vtune.h"
#include "../../../include/mxnet/operator.h"
#include "./test_op.h"
#include "./test_op_runner.h"

namespace mxnet {
namespace test {
namespace op {

/*!
 * \brief Manage test blobs and context, and universal logic
 * Create an operator from its "Prop" class and sets up the operator
 * and resources for both forward and backward passes
 * \tparam DType
 */
template <typename DType, typename AccReal>
class LegacyOperatorExecutor : public OperatorDataInitializer<DType>
                              , public OperatorExecutorTiming {
 public:
  typedef DType DataType;
  typedef AccReal AccRealType;

  /*! \brief Manage test blobs and context */
  LegacyOperatorExecutor(const bool isGPU, const std::vector<TShape>& topShapes)
#if !MXNET_USE_CUDA
    : isGPU_(false)
#else
    : isGPU_(isGPU)
#endif
    , initializeForward_(0)   // unit testing may call inits in any order based
      , initializeBackward_(0)  // upon its use-case (ie may not want to run forward pass first)
      , initializeCallback_(0) {
    opContext_.is_train = true;
    opContext_.run_ctx.stream = nullptr;
    CHECK(!topShapes.empty());
    shape_input_vec_ = topShapes;
  }

  inline mxnet::Context getContext() {
    return isGPU_ ? mxnet::Context::GPU(0) : mxnet::Context{};
  }

  /*! \brief Initialize forward blob data values */
  virtual void resetForward() {}

  /*! \brief Initialize backward blob data values */
  virtual void resetBackward() {}

  /*! \brief Initialize auxiliary and output blobs */
  template<typename OperatorPropertyType>
  bool initForward(const OperatorPropertyType &opProp, std::vector<int> *in_type) {
    if (!initializeForward_++) {
      shape_input_vec_.resize(opProp.ListArguments().size());
      op_.reset(opProp.CreateOperatorEx(getContext(), &shape_input_vec_, in_type));
      if (op_) {
        const size_t output_count = opProp.ListOutputs().size();
        const size_t aux_count = opProp.ListAuxiliaryStates().size();
        // Figure out what sort of blobs we need to allocate
        std::vector<TShape> out_shape, aux_shape;
        out_shape.resize(output_count);
        aux_shape.resize(aux_count);
        opProp.InferShape(&shape_input_vec_, &out_shape, &aux_shape);
        std::vector<int> out_type(output_count, -1), aux_type(aux_count, -1);
        opProp.InferType(in_type, &out_type, &aux_type);

        // Allocate top blobs (input)
        for (size_t x = 0, n = shape_input_vec_.size(); x < n; ++x) {
          int type;
          if (x < in_type->size()) {
            type = (*in_type)[x];
          } else {
            type = x ? mshadow::DataType<AccReal>::kFlag : mshadow::DataType<DType>::kFlag;
          }

          allocateBlob(&c_.blob_input_vec_, shape_input_vec_[x], false, type);
        }

        // Allocate aux blobs (scratch, hidden, etc.)
        for (size_t x = 0, n = aux_shape.size(); x < n; ++x) {
          CHECK(x < aux_type.size());
          allocateBlob(&c_.blob_aux_states_, aux_shape[x], false, aux_type[x]);
        }

        // Allocate bottom blobs (output)
        for (size_t x = 0, n = out_shape.size(); x < n; ++x) {
          CHECK(x < out_type.size());
          allocateBlob(&c_.blob_output_vec_, out_shape[x], false, out_type[x]);
        }

        // Get the resource of temporal space
        std::vector<TShape> inputShapes;
        for (size_t x = 0, n = shape_input_vec_.size(); x < n; ++x) {
          inputShapes.emplace_back(shape_input_vec_[x]);
        }
        allocateResources(opProp.ForwardResource(inputShapes));

        resetForward();
        return true;
      }
      return false;
    } else {
      return true;
    }
  }

  /*! \brief Initialize auxiliary and output blobs */
  template<typename OperatorPropertyType>
  bool initBackward(const OperatorPropertyType &opProp, std::vector<int> *in_type) {
    initForward(opProp, in_type);
    if (!initializeBackward_++) {
      for (size_t x = 0, n = static_cast<size_t>(opProp.NumVisibleOutputs()); x < n; ++x) {
        CHECK_LT(x, c_.blob_output_vec_.size());
        allocateBlob(&c_.blob_out_grad_, c_.blob_output_vec_[x].shape_,
                     false, c_.blob_output_vec_[x].type_flag_);
      }

      for (size_t x = 0, n = c_.blob_input_vec_.size(); x < n; ++x) {
        allocateBlob(&c_.blob_in_grad_,  c_.blob_input_vec_[x].shape_,
                     false, c_.blob_input_vec_[x].type_flag_);
      }

      // Get the resource of temporal space
      std::vector<TShape> ishapes;
      allocateResources(opProp.BackwardResource(ishapes));

      resetBackward();
      return false;
    } else {
      return true;
    }
  }

  /*! \brief Run operator forward */
  void forward(const size_t count = 1) {
    const std::vector<OpReqType> req(c_.blob_output_vec_.size(), kWriteTo);
    // Possibly move data to/from CPU and GPU (outside of timing scope)
    MXNET_CUDA_ONLY(std::unique_ptr<GPUOpData> gpuData(isGPU_ ?
                      new GPUOpData(c_, &opContext_) : nullptr));
    perf::TimingItem timeF(&OperatorExecutorTiming::GetTiming(), Forward,
                           "Forward", count);
    if (!isGPU_) {
      mxnet::profiler::vtune::VTuneResume profile;  // VTune sample only this scope
      for (size_t x = 0; x < count; ++x) {
        op()->Forward(opContext_,
                      c_.blob_input_vec_,
                      req,
                      c_.blob_output_vec_,
                      c_.blob_aux_states_);
      }
    } else {
      for (size_t x = 0; x < count; ++x) {
        MXNET_CUDA_ONLY(op()->Forward(opContext_,
                                      gpuData->blob_input_vec_,
                                      req,
                                      gpuData->blob_output_vec_,
                                      gpuData->blob_aux_states_));
      }
    }
  }

  /*! \brief Run operator backwards */
  void backward(const size_t count = 1) {
    const std::vector<OpReqType> req(c_.blob_in_grad_.size(), kWriteTo);
    // Possibly move data to/from CPU and GPU (outside of timing scope)
    MXNET_CUDA_ONLY(std::unique_ptr<GPUOpData> gpuData(isGPU_ ?
                      new GPUOpData(c_, &opContext_) : nullptr));
    perf::TimingItem timeB(&OperatorExecutorTiming::GetTiming(), Backward,
                           "Backward", count);
    if (!isGPU_) {
      mxnet::profiler::vtune::VTuneResume profile;  // VTune sample only this scope
      for (size_t x = 0; x < count; ++x) {
        op()->Backward(opContext_,
                       c_.blob_out_grad_,
                       c_.blob_input_vec_,
                       c_.blob_output_vec_,
                       req,
                       c_.blob_in_grad_,
                       c_.blob_aux_states_);
      }
    } else {
      for (size_t x = 0; x < count; ++x) {
        MXNET_CUDA_ONLY(op()->Backward(opContext_,
                                       gpuData->blob_out_grad_,
                                       gpuData->blob_input_vec_,
                                       gpuData->blob_output_vec_,
                                       req,
                                       gpuData->blob_in_grad_,
                                       gpuData->blob_aux_states_));
      }
    }
  }

  /*!
   * \brief Test if operator has a backward pass
   * \return true if this operator has a backward pass
   */
  MSHADOW_CINLINE bool HasBackward() const { return true; }

  /*! \brief Getter functions for the operator */
  inline Operator *op() { return op_.get(); }
  inline const Operator *op() const { return op_.get(); }

  enum BlobVectorType {
    kInput,
    kOutput,
    kAux,
    kInGrad,
    kOutGrad,
    kBlobVectorTypeCount
  };

#define CASE_STR(__v$) case (__v$): return #__v$

  /*! \brief Convert BlobVectorType enum into a string */
  static inline const char *bvt2String(const BlobVectorType bvt) {
    switch (bvt) {
      CASE_STR(kInput);
      CASE_STR(kOutput);
      CASE_STR(kAux);
      CASE_STR(kInGrad);
      CASE_STR(kOutGrad);
      default:
        CHECK(false);
        return "";
    }
  }
#undef CASE_STR

  /*! \brief Return a particular blob in a test data set */
  inline const std::vector<TBlob>& getBlobVect(const BlobVectorType bvt) const {
    switch (bvt) {
      case kInput:
        return c_.blob_input_vec_;
      case kOutput:
        return c_.blob_output_vec_;
      case kAux:
        return c_.blob_aux_states_;
      case kInGrad:
        return c_.blob_in_grad_;
      case kOutGrad:
        return c_.blob_out_grad_;
      default:
        CHECK(false);
        return c_.blob_input_vec_;
    }
  }

  /*! \brief Dump an operator's data set into compilable C++ data code for runtime validation
   * When writing an operator test, you can generate a "known good operator data state" in C++
   * code with this function, and then use load() to load the blob states into this
   * class (BasicOperatorData).
   * After that, you can compare with the "actual" operator state (BasicOperatorData) of
   * the operator that you are testing.
   */
  template<typename Stream>
  inline void dumpC(Stream *_os, const std::string& label) {
    Stream& os = *_os;
    os << "static const std::vector< std::vector< std::vector<float> > > ___"
       << label << "_data_shape_";
    const TShape& shape = shape_input_vec_[0];
    for (size_t i = 0, n = shape.ndim(); i < n; ++i) {
      os << shape[i] << "_";
    }
    os << "__ =" << std::endl << "{" << std::endl;
    for (size_t x = 0; x < kBlobVectorTypeCount; ++x) {
      os << "  { /* " << bvt2String(BlobVectorType(x)) << " */" << std::endl;
      const std::vector<TBlob>& blobVect = getBlobVect(BlobVectorType(x));
      for (size_t i = 0, n = blobVect.size(); i < n; ++i) {
        os << "    { ";
        test::dump<DType>(&os, blobVect[i]);
        os << " }";
        if (i < n - 1) {
          os << ",";
        }
        os << std::endl;
      }
      os << "  }";
      if (x < kBlobVectorTypeCount - 1) {
        os << ",";
      }
      os << std::endl;
    }
    os << "};" << std::endl;
  }

  static inline void copy(const TBlob& blob, const DType array[],
                          const size_t start, const size_t end) {
    const size_t blobSize = blob.Size();
    DType *p = blob.dptr<DType>();
    for (size_t i = 0, n = end - start; i < n; ++i) {
      CHECK_LT(i, blobSize);
      p[i] = array[i + start];
    }
  }

  /*! \brief Runtime load of the C++ data code generated by dumpC() */
  void load(const std::vector<std::vector<std::vector<DType>>>& cData) {
    for (size_t i = 0, ni = cData.size(); i < ni; ++i) {
      for (size_t j = 0, nj = cData[i].size(); j < nj; ++j)  {
        const TBlob& blob = getBlobVect(BlobVectorType(i))[j];
        const size_t sourceDataSize = cData[i][j].size();
        CHECK_EQ(sourceDataSize, blob.Size());
        const DType *sourceData = &cData[i][j][0];
        copy(blob, sourceData, 0, sourceDataSize);
      }
    }
  }

  /*! \brief Runtime load of the C++ data code generated by dumpC() */
  void load(const std::vector<std::vector<std::vector<DType>>>& cData,
            const BlobVectorType type) {
    CHECK_LT(type, cData.size());
    for (size_t j = 0, nj = cData[type].size(); j < nj; ++j)  {
      const TBlob& blob = getBlobVect(type)[j];
      const size_t sourceDataSize = cData[type][j].size();
      CHECK_EQ(sourceDataSize, blob.Size());
      const DType *sourceData = &cData[type][j][0];
      copy(blob, sourceData, 0, sourceDataSize);
    }
  }

  /*! \brief Runtime load of the C++ data code generated by dumpC() */
  void load(const std::vector<std::vector<std::vector<DType>>>& cData,
            const BlobVectorType type, const int idx) {
    CHECK_LT(type, cData.size());
    CHECK_LT(idx, cData[type].size());
    const TBlob& blob = getBlobVect(type)[idx];
    const size_t sourceDataSize = cData[type][idx].size();
    CHECK_EQ(sourceDataSize, blob.Size());
    const DType *sourceData = &cData[type][idx][0];
    copy(blob, sourceData, 0, sourceDataSize);
  }

//  void FillRandom() {
//    for (size_t j = 0, jn = this->c_.all_blob_vects_.size(); j < jn; ++j) {
//      std::vector<TBlob> *data_vect = this->c_.all_blob_vects_[j];
//      if (data_vect) {
//        for (size_t i = 0, n = data_vect->size(); i < n; ++i) {
//          OperatorDataInitializer<DType>::FillRandom((*data_vect)[i]);
//        }
//      }
//    }
//  }

  std::vector<TBlob>& inputs() { return c_.blob_input_vec_; }
  const std::vector<TBlob>& inputs() const { return c_.blob_input_vec_; }
  std::vector<TBlob>& outputs() { return c_.blob_output_vec_; }
  const std::vector<TBlob>& outputs() const { return c_.blob_output_vec_; }
  std::vector<TBlob>& bwd_inputs() { return c_.blob_out_grad_; }
  std::vector<TBlob>& bwd_outputs() { return c_.blob_in_grad_; }

  /*! \brief Input and output blobs */
  OpContext                 opContext_;

  std::vector<TShape>       shape_input_vec_;

  struct OpData {
    std::vector<TBlob> blob_input_vec_;
    std::vector<TBlob> blob_output_vec_;
    std::vector<TBlob> blob_aux_states_;
    std::vector<TBlob> blob_in_grad_;
    std::vector<TBlob> blob_out_grad_;  // Remaining err (loss) pushing back upstream

    std::vector<std::vector<TBlob> *> all_blob_vects_;
    inline OpData() {
      all_blob_vects_.emplace_back(&blob_input_vec_);
      all_blob_vects_.emplace_back(&blob_output_vec_);
      all_blob_vects_.emplace_back(&blob_aux_states_);
      all_blob_vects_.emplace_back(&blob_in_grad_);
      all_blob_vects_.emplace_back(&blob_out_grad_);  // Remaining err (loss) pushing back upstream
    }
    virtual ~OpData() {}
  };

#if MXNET_USE_CUDA
  class GPUOpData : public OpData {
    GPUOpData() = delete;
    GPUOpData(const GPUOpData& o) = delete;

   public:
    inline GPUOpData(const OpData& cpuData, OpContext *opContext)
    : cpuData_(cpuData)
      , allocGPUStream_(opContext) {
      // Copy CPU->GPU
      CHECK_EQ(gpuBlobs_.size(), 0U);
      CHECK_EQ(cpuData_.all_blob_vects_.size(), this->all_blob_vects_.size());
      for (size_t bvt = 0, nbvt = cpuData_.all_blob_vects_.size(); bvt < nbvt; ++bvt) {
        std::vector<TBlob>& bv_src = *cpuData_.all_blob_vects_[bvt];
        std::vector<TBlob>& bvt_dest = *this->all_blob_vects_[bvt];
        for (size_t i = 0, n = bv_src.size(); i < n; ++i) {
          const TBlob& srcBlob = bv_src[i];
          TBlob *destBlob = allocateBlob(&gpuBlobs_, &bvt_dest, srcBlob.shape_,
                                         true, srcBlob.type_flag_);

          Context cpu_ctx, gpu_ctx;
          cpu_ctx.dev_type = Context::kCPU;
          gpu_ctx.dev_type = Context::kGPU;
          cpu_ctx.dev_id = gpu_ctx.dev_id = 0;

          mxnet::ndarray::Copy<cpu, gpu>(srcBlob, destBlob, cpu_ctx,
                                         gpu_ctx, allocGPUStream_.opContext_.run_ctx);
        }
      }
      cudaDeviceSynchronize();
    }
    inline ~GPUOpData() {
      // Copy GPU->CPU
      cudaDeviceSynchronize();
      for (size_t bvt = 0, nbvt = this->all_blob_vects_.size(); bvt < nbvt; ++bvt) {
        std::vector<TBlob>& bv_src = *this->all_blob_vects_[bvt];
        std::vector<TBlob>& bvt_dest = *cpuData_.all_blob_vects_[bvt];
        for (size_t i = 0, n = bv_src.size(); i < n; ++i) {
          const TBlob& srcBlob = bv_src[i];
          TBlob *destBlob = &bvt_dest[i];

          Context cpu_ctx, gpu_ctx;
          cpu_ctx.dev_type = Context::kCPU;
          gpu_ctx.dev_type = Context::kGPU;
          cpu_ctx.dev_id = gpu_ctx.dev_id = 0;

          mxnet::ndarray::Copy<gpu, cpu>(srcBlob, destBlob, gpu_ctx,
                                         cpu_ctx, allocGPUStream_.opContext_.run_ctx);
        }
      }
      gpuBlobs_.clear();  // Force deallocation of the GPU blob data
      cudaDeviceSynchronize();
    }

   private:
    /*! \brief Reference to the src/dest CPU data */
    const OpData& cpuData_;
    /*! \brief The GPU-allocated blobs */
    std::list<std::unique_ptr<test::StandaloneBlob>> gpuBlobs_;
    /*! \brief Scoped GPU stream */
    GPUStreamScope allocGPUStream_;
  };
#endif  // MXNET_USE_CUDA

 protected:
  OpData                    c_;

  /*! \brief Allocate the operator's resource requests */
  void allocateResources(const std::vector<ResourceRequest>& reqs) {
    std::map<Context, Resource> cached_temp;

    Context ctx;
    ctx.dev_type = isGPU_ ? Context::kGPU : Context::kCPU;
    ctx.dev_id = 0;

    for (const ResourceRequest& req : reqs) {
      switch (req.type) {
        case ResourceRequest::kTempSpace: {
          if (cached_temp.count(ctx) != 0) {
            opContext_.requested.emplace_back(cached_temp.at(ctx));
          } else {
            Resource r = ResourceManager::Get()->Request(ctx, req);
            opContext_.requested.emplace_back(r);
            cached_temp[ctx] = r;
          }
          break;
        }
        case ResourceRequest::kRandom: {
          opContext_.requested.emplace_back(ResourceManager::Get()->Request(ctx, req));
          break;
        }
        case ResourceRequest::kParallelRandom: {
          Resource rm = ResourceManager::Get()->Request(ctx, req);
          if (ctx.dev_mask() == Context::kCPU) {
            common::random::RandGenerator<cpu, DType>::AllocState(
              rm.get_parallel_random<cpu, DType>());
          }
          opContext_.requested.emplace_back(rm);
          break;
        }
#if MXNET_USE_CUDNN == 1 && CUDNN_MAJOR >= 7
        case ResourceRequest::kCuDNNDropoutDesc: {
          opContext_.requested.push_back(ResourceManager::Get()->Request(ctx, req));
          break;
        }
#endif  // MXNET_USE_CUDNN == 1 && CUDNN_MAJOR >= 7
        default:
          LOG(FATAL) << "resource type " << req.type << " is not yet supported";
      }
    }
  }

  /*! \brief Locally allocate a managed TBlob and insert into the supplied vector */
  static TBlob *allocateBlob(std::list<std::unique_ptr<test::StandaloneBlob>> *standalone_blobs,
                             std::vector<TBlob> *dest,
                             const TShape& shape,
                             const bool isGPU,
                             const int dtype) {
    test::StandaloneBlob *blob = new test::StandaloneBlob(shape, isGPU, dtype);
    CHECK_NE(blob, static_cast<TBlob *>(nullptr));
    standalone_blobs->emplace_back(std::unique_ptr<test::StandaloneBlob>(blob));
    (*dest).emplace_back(*blob);
    return blob;
  }

  /*! \brief Locally allocate a managed TBlob and insert into the supplied vector */
  inline TBlob *allocateBlob(std::vector<TBlob> *dest, const TShape& shape,
                             const bool isGPU, const int dtype) {
    return allocateBlob(&standalone_blobs_, dest, shape, isGPU, dtype);
  }

  /*! \brief Performance timing categories */
  enum TimingId {
    Forward,
    Backward
  };

  /*! \brief The operator */
  std::unique_ptr<Operator>   op_;
  /*! \brief Is this for a GPU? */
  const bool                  isGPU_;
  /*! \brief Assure that the Forward initialized only once */
  std::atomic<int>            initializeForward_;
  /*! \brief Assure that the Forward initialized only once */
  std::atomic<int>            initializeBackward_;
  /*! \brief Assure that the callback is initialized only once */
  std::atomic<int>            initializeCallback_;
  /*! \brief scoped lifecycle management of allocated blobs */
  std::list<std::unique_ptr<test::StandaloneBlob>> standalone_blobs_;
};

template<typename OperatorProp, typename DType, typename AccReal>
using LegacyOpRunner =
mxnet::test::OperatorRunner<OperatorProp, LegacyOperatorExecutor<DType, AccReal>>;

}  // namespace op
}  // namespace test
}  // namespace mxnet

#endif  // TEST_LEGACY_OP_H_
