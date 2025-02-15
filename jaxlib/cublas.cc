/* Copyright 2019 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "include/pybind11/numpy.h"
#include "include/pybind11/pybind11.h"
#include "include/pybind11/stl.h"
#include "jaxlib/cuda_gpu_kernel_helpers.h"
#include "jaxlib/handle_pool.h"
#include "jaxlib/kernel_pybind11_helpers.h"
#include "third_party/gpus/cuda/include/cublas_v2.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "third_party/gpus/cuda/include/cuda_runtime_api.h"

namespace jax {
namespace {

namespace py = pybind11;

using BlasHandlePool = HandlePool<cublasHandle_t, cudaStream_t>;

template <>
/*static*/ BlasHandlePool::Handle BlasHandlePool::Borrow(cudaStream_t stream) {
  BlasHandlePool* pool = Instance();
  absl::MutexLock lock(&pool->mu_);
  cublasHandle_t handle;
  if (pool->handles_[stream].empty()) {
    JAX_THROW_IF_ERROR(cublasCreate(&handle));
  } else {
    handle = pool->handles_[stream].back();
    pool->handles_[stream].pop_back();
  }
  if (stream) {
    JAX_THROW_IF_ERROR(cublasSetStream(handle, stream));
  }
  return Handle(pool, handle, stream);
}

// Set of types known to Cusolver.
enum class Type {
  F32,
  F64,
  C64,
  C128,
};

// Converts a NumPy dtype to a Type.
Type DtypeToType(const py::dtype& np_type) {
  static auto* types = new absl::flat_hash_map<std::pair<char, int>, Type>({
      {{'f', 4}, Type::F32},
      {{'f', 8}, Type::F64},
      {{'c', 8}, Type::C64},
      {{'c', 16}, Type::C128},
  });
  auto it = types->find({np_type.kind(), np_type.itemsize()});
  if (it == types->end()) {
    throw std::invalid_argument(
        absl::StrFormat("Unsupported dtype %s", py::repr(np_type)));
  }
  return it->second;
}

int SizeOfType(Type type) {
  switch (type) {
    case Type::F32:
      return sizeof(float);
    case Type::F64:
      return sizeof(double);
    case Type::C64:
      return sizeof(cuComplex);
    case Type::C128:
      return sizeof(cuDoubleComplex);
  }
}

// Batched triangular solve: trsmbatched

struct TrsmBatchedDescriptor {
  Type type;
  int batch, m, n;
  cublasSideMode_t side;
  cublasFillMode_t uplo;
  cublasOperation_t trans;
  cublasDiagType_t diag;
};

// Returns the descriptor for a TrsmBatched operation.
std::pair<size_t, py::bytes> BuildTrsmBatchedDescriptor(
    const py::dtype& dtype, int batch, int m, int n, bool left_side, bool lower,
    bool trans_a, bool conj_a, bool unit_diagonal) {
  size_t size = batch * sizeof(void*);
  TrsmBatchedDescriptor desc;
  desc.type = DtypeToType(dtype);
  desc.batch = batch;
  desc.m = m;
  desc.n = n;
  desc.side = left_side ? CUBLAS_SIDE_LEFT : CUBLAS_SIDE_RIGHT;
  desc.uplo = lower ? CUBLAS_FILL_MODE_LOWER : CUBLAS_FILL_MODE_UPPER;
  desc.trans = trans_a ? (conj_a ? CUBLAS_OP_C : CUBLAS_OP_T) : CUBLAS_OP_N;
  desc.diag = unit_diagonal ? CUBLAS_DIAG_UNIT : CUBLAS_DIAG_NON_UNIT;
  return {size, PackDescriptor(desc)};
}

void TrsmBatched(cudaStream_t stream, void** buffers, const char* opaque,
                 size_t opaque_len) {
  const TrsmBatchedDescriptor& d =
      *UnpackDescriptor<TrsmBatchedDescriptor>(opaque, opaque_len);
  auto handle = BlasHandlePool::Borrow(stream);
  if (buffers[2] != buffers[1]) {
    JAX_THROW_IF_ERROR(cudaMemcpyAsync(buffers[2], buffers[1],
                                       SizeOfType(d.type) * d.batch * d.m * d.n,
                                       cudaMemcpyDeviceToDevice, stream));
  }
  const int lda = d.side == CUBLAS_SIDE_LEFT ? d.m : d.n;
  const int ldb = d.m;
  auto a_batch_host = MakeBatchPointers(stream, buffers[0], buffers[3], d.batch,
                                        SizeOfType(d.type) * lda * lda);
  auto b_batch_host = MakeBatchPointers(stream, buffers[2], buffers[4], d.batch,
                                        SizeOfType(d.type) * d.m * d.n);
  // TODO(phawkins): ideally we would not need to synchronize here, but to
  // avoid it we need a way to keep the host-side buffer alive until the copy
  // completes.
  JAX_THROW_IF_ERROR(cudaStreamSynchronize(stream));
  switch (d.type) {
    case Type::F32: {
      float* a = static_cast<float*>(buffers[0]);
      float* b = static_cast<float*>(buffers[2]);
      float** a_batch_ptrs = static_cast<float**>(buffers[3]);
      float** b_batch_ptrs = static_cast<float**>(buffers[4]);
      // NOTE(phawkins): if alpha is in GPU memory, cuBlas seems to segfault.
      const float alpha = 1.0f;
      JAX_THROW_IF_ERROR(cublasStrsmBatched(
          handle.get(), d.side, d.uplo, d.trans, d.diag, d.m, d.n, &alpha,
          const_cast<const float**>(a_batch_ptrs), lda, b_batch_ptrs, ldb,
          d.batch));
      break;
    }
    case Type::F64: {
      double* a = static_cast<double*>(buffers[0]);
      double* b = static_cast<double*>(buffers[2]);
      double** a_batch_ptrs = static_cast<double**>(buffers[3]);
      double** b_batch_ptrs = static_cast<double**>(buffers[4]);
      const double alpha = 1.0;
      JAX_THROW_IF_ERROR(cublasDtrsmBatched(
          handle.get(), d.side, d.uplo, d.trans, d.diag, d.m, d.n, &alpha,
          const_cast<const double**>(a_batch_ptrs), lda, b_batch_ptrs, ldb,
          d.batch));
      break;
    }
    case Type::C64: {
      cuComplex* a = static_cast<cuComplex*>(buffers[0]);
      cuComplex* b = static_cast<cuComplex*>(buffers[2]);
      cuComplex** a_batch_ptrs = static_cast<cuComplex**>(buffers[3]);
      cuComplex** b_batch_ptrs = static_cast<cuComplex**>(buffers[4]);
      const cuComplex alpha = make_cuComplex(1.0f, 0.0f);
      JAX_THROW_IF_ERROR(cublasCtrsmBatched(
          handle.get(), d.side, d.uplo, d.trans, d.diag, d.m, d.n, &alpha,
          const_cast<const cuComplex**>(a_batch_ptrs), lda, b_batch_ptrs, ldb,
          d.batch));
      break;
    }
    case Type::C128: {
      cuDoubleComplex* a = static_cast<cuDoubleComplex*>(buffers[0]);
      cuDoubleComplex* b = static_cast<cuDoubleComplex*>(buffers[2]);
      cuDoubleComplex** a_batch_ptrs =
          static_cast<cuDoubleComplex**>(buffers[3]);
      cuDoubleComplex** b_batch_ptrs =
          static_cast<cuDoubleComplex**>(buffers[4]);
      const cuDoubleComplex alpha = make_cuDoubleComplex(1.0f, 0.0f);
      JAX_THROW_IF_ERROR(cublasZtrsmBatched(
          handle.get(), d.side, d.uplo, d.trans, d.diag, d.m, d.n, &alpha,
          const_cast<const cuDoubleComplex**>(a_batch_ptrs), lda, b_batch_ptrs,
          ldb, d.batch));
      break;
    }
  }
}

// Batched LU decomposition: getrfbatched

struct GetrfBatchedDescriptor {
  Type type;
  int batch, n;
};

// Returns the descriptor for a GetrfBatched operation.
std::pair<size_t, py::bytes> BuildGetrfBatchedDescriptor(const py::dtype& dtype,
                                                         int b, int n) {
  Type type = DtypeToType(dtype);
  size_t size = b * sizeof(void*);
  return {size, PackDescriptor(GetrfBatchedDescriptor{type, b, n})};
}

void GetrfBatched(cudaStream_t stream, void** buffers, const char* opaque,
                  size_t opaque_len) {
  const GetrfBatchedDescriptor& d =
      *UnpackDescriptor<GetrfBatchedDescriptor>(opaque, opaque_len);
  auto handle = BlasHandlePool::Borrow(stream);
  if (buffers[0] != buffers[1]) {
    JAX_THROW_IF_ERROR(cudaMemcpyAsync(buffers[1], buffers[0],
                                       SizeOfType(d.type) * d.batch * d.n * d.n,
                                       cudaMemcpyDeviceToDevice, stream));
  }

  int* ipiv = static_cast<int*>(buffers[2]);
  int* info = static_cast<int*>(buffers[3]);
  auto a_ptrs_host = MakeBatchPointers(stream, buffers[1], buffers[4], d.batch,
                                       SizeOfType(d.type) * d.n * d.n);
  // TODO(phawkins): ideally we would not need to synchronize here, but to
  // avoid it we need a way to keep the host-side buffer alive until the copy
  // completes.
  JAX_THROW_IF_ERROR(cudaStreamSynchronize(stream));
  switch (d.type) {
    case Type::F32: {
      float* a = static_cast<float*>(buffers[1]);
      float** batch_ptrs = static_cast<float**>(buffers[4]);
      JAX_THROW_IF_ERROR(cublasSgetrfBatched(handle.get(), d.n, batch_ptrs, d.n,
                                             ipiv, info, d.batch));
      break;
    }
    case Type::F64: {
      double* a = static_cast<double*>(buffers[1]);
      double** batch_ptrs = static_cast<double**>(buffers[4]);
      JAX_THROW_IF_ERROR(cublasDgetrfBatched(handle.get(), d.n, batch_ptrs, d.n,
                                             ipiv, info, d.batch));
      break;
    }
    case Type::C64: {
      cuComplex* a = static_cast<cuComplex*>(buffers[1]);
      cuComplex** batch_ptrs = static_cast<cuComplex**>(buffers[4]);
      JAX_THROW_IF_ERROR(cublasCgetrfBatched(handle.get(), d.n, batch_ptrs, d.n,
                                             ipiv, info, d.batch));
      break;
    }
    case Type::C128: {
      cuDoubleComplex* a = static_cast<cuDoubleComplex*>(buffers[1]);
      cuDoubleComplex** batch_ptrs = static_cast<cuDoubleComplex**>(buffers[4]);
      JAX_THROW_IF_ERROR(cublasZgetrfBatched(handle.get(), d.n, batch_ptrs, d.n,
                                             ipiv, info, d.batch));
      break;
    }
  }
}

py::dict Registrations() {
  py::dict dict;
  dict["cublas_trsm_batched"] = EncapsulateFunction(TrsmBatched);
  dict["cublas_getrf_batched"] = EncapsulateFunction(GetrfBatched);
  return dict;
}

PYBIND11_MODULE(cublas_kernels, m) {
  m.def("registrations", &Registrations);
  m.def("build_trsm_batched_descriptor", &BuildTrsmBatchedDescriptor);
  m.def("build_getrf_batched_descriptor", &BuildGetrfBatchedDescriptor);
}

}  // namespace
}  // namespace jax
