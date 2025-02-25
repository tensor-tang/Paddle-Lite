// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lite/backends/opencl/target_wrapper.h"
#include <algorithm>
#include <array>
#include "lite/backends/opencl/cl_include.h"
#include "lite/backends/opencl/cl_runtime.h"
#include "lite/backends/opencl/cl_utility.h"

namespace paddle {
namespace lite {

static cl_channel_type GetCLChannelType(const PrecisionType type) {
  switch (type) {
    case PRECISION(kFloat):
      return CL_FLOAT;
    case PRECISION(kInt32):
      return CL_SIGNED_INT32;
    case PRECISION(kInt8):
      return CL_SIGNED_INT8;
    default:
      LOG(FATAL) << "Unsupported image channel type: " << PrecisionToStr(type);
      return 0;
  }
}

void *TargetWrapperCL::Malloc(size_t size) {
  cl_int status;
  cl::Buffer *buffer = new cl::Buffer(CLRuntime::Global()->context(),
                                      CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR,
                                      size,
                                      nullptr,
                                      &status);
  if (status != CL_SUCCESS) {
    delete buffer;
    buffer = nullptr;
  }
  CL_CHECK_FATAL(status);
  return buffer;
}

void TargetWrapperCL::Free(void *ptr) {
  if (ptr != nullptr) {
    cl::Buffer *cl_buffer = static_cast<cl::Buffer *>(ptr);
    delete cl_buffer;
  }
}

void *TargetWrapperCL::MallocImage(const std::array<size_t, 2> &image_shape,
                                   PrecisionType data_type) {
  cl::ImageFormat img_format(CL_RGBA, GetCLChannelType(data_type));
  cl_int status;
  size_t width = image_shape[0];
  size_t height = image_shape[1];
  cl::Image2D *cl_image =
      new cl::Image2D(CLRuntime::Global()->context(),
                      CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR,
                      img_format,
                      width,
                      height,
                      0,
                      nullptr,
                      &status);
  if (status != CL_SUCCESS) {
    delete cl_image;
    cl_image = nullptr;
  }
  CL_CHECK_FATAL(status);
  return cl_image;
}

void TargetWrapperCL::FreeImage(void *image) {
  if (image != nullptr) {
    cl::Image2D *cl_image = static_cast<cl::Image2D *>(image);
    delete cl_image;
  }
}

void *TargetWrapperCL::Map(void *buffer, size_t offset, size_t size) {
  cl::Buffer *cl_buffer = static_cast<cl::Buffer *>(buffer);
  cl_int status;
  void *mapped_ptr = CLRuntime::Global()->command_queue().enqueueMapBuffer(
      *cl_buffer,
      CL_TRUE,
      CL_MAP_READ | CL_MAP_WRITE,
      offset,
      size,
      nullptr,
      nullptr,
      &status);
  if (status != CL_SUCCESS) {
    mapped_ptr = nullptr;
  }
  CL_CHECK_FATAL(status);
  return mapped_ptr;
}

void *TargetWrapperCL::MapImage(void *image,
                                const std::array<size_t, 2> &image_shape,
                                std::array<size_t, 2> *image_pitch) {
  cl::Image2D *cl_image = static_cast<cl::Image2D *>(image);
  size_t width = image_shape[0];
  size_t height = image_shape[1];
  size_t *row_pitch = image_pitch->data();
  size_t *slice_pitch = image_pitch->data() + 1;
  std::array<size_t, 3> origin{{0, 0, 0}};
  std::array<size_t, 3> region{{width, height, 1}};
  cl_int status;
  void *mapped_ptr = CLRuntime::Global()->command_queue().enqueueMapImage(
      *cl_image,
      CL_TRUE,
      CL_MAP_READ | CL_MAP_WRITE,
      origin,
      region,
      row_pitch,
      slice_pitch,
      nullptr,
      nullptr,
      &status);
  if (status != CL_SUCCESS) {
    mapped_ptr = nullptr;
  }
  CL_CHECK_FATAL(status);
  return mapped_ptr;
}

void TargetWrapperCL::Unmap(void *cl_obj, void *mapped_ptr) {
  cl::Memory *mem_obj = static_cast<cl::Memory *>(cl_obj);
  cl_int status = CLRuntime::Global()->command_queue().enqueueUnmapMemObject(
      *mem_obj, mapped_ptr, nullptr, nullptr);
  CL_CHECK_FATAL(status);
}

void TargetWrapperCL::MemcpySync(void *dst,
                                 const void *src,
                                 size_t size,
                                 IoDirection dir) {
  cl_int status;
  cl::Event event;
  auto stream = CLRuntime::Global()->command_queue();
  switch (dir) {
    case IoDirection::DtoD:
      status = stream.enqueueCopyBuffer(*static_cast<const cl::Buffer *>(src),
                                        *static_cast<cl::Buffer *>(dst),
                                        0,
                                        0,
                                        size,
                                        nullptr,
                                        &event);
      CL_CHECK_FATAL(status);
      event.wait();
      break;
    case IoDirection::HtoD:
      status = stream.enqueueWriteBuffer(*static_cast<cl::Buffer *>(dst),
                                         CL_TRUE,
                                         0,
                                         size,
                                         src,
                                         nullptr,
                                         nullptr);
      CL_CHECK_FATAL(status);
      break;
    case IoDirection::DtoH:
      status = stream.enqueueReadBuffer(*static_cast<const cl::Buffer *>(src),
                                        CL_TRUE,
                                        0,
                                        size,
                                        dst,
                                        nullptr,
                                        nullptr);
      CL_CHECK_FATAL(status);
      break;
    default:
      LOG(FATAL) << "Unsupported IoDirection " << static_cast<int>(dir);
  }
}

void TargetWrapperCL::MemcpyAsync(void *dst,
                                  const void *src,
                                  size_t size,
                                  IoDirection dir,
                                  const stream_t &stream) {
  cl_int status;
  switch (dir) {
    case IoDirection::DtoD:
      status = stream.enqueueCopyBuffer(*static_cast<const cl::Buffer *>(src),
                                        *static_cast<cl::Buffer *>(dst),
                                        0,
                                        0,
                                        size,
                                        nullptr,
                                        nullptr);
      CL_CHECK_FATAL(status);
      break;
    case IoDirection::HtoD:
      status = stream.enqueueWriteBuffer(*static_cast<cl::Buffer *>(dst),
                                         CL_FALSE,
                                         0,
                                         size,
                                         src,
                                         nullptr,
                                         nullptr);
      CL_CHECK_FATAL(status);
      break;
    case IoDirection::DtoH:
      status = stream.enqueueReadBuffer(*static_cast<const cl::Buffer *>(src),
                                        CL_FALSE,
                                        0,
                                        size,
                                        dst,
                                        nullptr,
                                        nullptr);
      CL_CHECK_FATAL(status);
      break;
    default:
      LOG(FATAL) << "Unsupported IoDirection " << static_cast<int>(dir);
  }
}

void TargetWrapperCL::ImgcpySync(void *dst,
                                 const void *src,
                                 const std::array<size_t, 2> &image_shape,
                                 const std::array<size_t, 2> &image_pitch,
                                 IoDirection dir) {
  size_t width = image_shape[0];
  size_t height = image_shape[1];
  size_t row_pitch = image_pitch[0];
  size_t slice_pitch = image_pitch[1];
  std::array<size_t, 3> origin{{0, 0, 0}};
  std::array<size_t, 3> region{{width, height, 1}};
  cl_int status;
  cl::Event event;
  auto stream = CLRuntime::Global()->command_queue();
  switch (dir) {
    case IoDirection::DtoD:
      status = stream.enqueueCopyImage(*static_cast<const cl::Image2D *>(src),
                                       *static_cast<cl::Image2D *>(dst),
                                       origin,
                                       origin,
                                       region,
                                       nullptr,
                                       &event);
      CL_CHECK_FATAL(status);
      event.wait();
      break;
    case IoDirection::HtoD:
      status = stream.enqueueWriteImage(*static_cast<cl::Image2D *>(dst),
                                        CL_TRUE,
                                        origin,
                                        region,
                                        row_pitch,
                                        slice_pitch,
                                        src,
                                        nullptr,
                                        nullptr);
      CL_CHECK_FATAL(status);
      break;
    case IoDirection::DtoH:
      status = stream.enqueueReadImage(*static_cast<const cl::Image2D *>(src),
                                       CL_TRUE,
                                       origin,
                                       region,
                                       row_pitch,
                                       slice_pitch,
                                       dst,
                                       nullptr,
                                       nullptr);
      CL_CHECK_FATAL(status);
      break;
    default:
      LOG(FATAL) << "Unsupported IoDirection " << static_cast<int>(dir);
  }
}

void TargetWrapperCL::ImgcpyAsync(void *dst,
                                  const void *src,
                                  const std::array<size_t, 2> &image_shape,
                                  const std::array<size_t, 2> &image_pitch,
                                  IoDirection dir,
                                  const stream_t &stream) {
  size_t width = image_shape[0];
  size_t height = image_shape[1];
  size_t row_pitch = image_pitch[0];
  size_t slice_pitch = image_pitch[1];
  std::array<size_t, 3> origin{{0, 0, 0}};
  std::array<size_t, 3> region{{width, height, 1}};
  cl_int status;
  switch (dir) {
    case IoDirection::DtoD:
      status = stream.enqueueCopyImage(*static_cast<const cl::Image2D *>(src),
                                       *static_cast<cl::Image2D *>(dst),
                                       origin,
                                       origin,
                                       region,
                                       nullptr,
                                       nullptr);
      CL_CHECK_FATAL(status);
      break;
    case IoDirection::HtoD:
      status = stream.enqueueWriteImage(*static_cast<cl::Image2D *>(dst),
                                        CL_FALSE,
                                        origin,
                                        region,
                                        row_pitch,
                                        slice_pitch,
                                        src,
                                        nullptr,
                                        nullptr);
      CL_CHECK_FATAL(status);
      break;
    case IoDirection::DtoH:
      status = stream.enqueueReadImage(*static_cast<const cl::Image2D *>(src),
                                       CL_FALSE,
                                       origin,
                                       region,
                                       row_pitch,
                                       slice_pitch,
                                       dst,
                                       nullptr,
                                       nullptr);
      CL_CHECK_FATAL(status);
      break;
    default:
      LOG(FATAL) << "Unsupported IoDirection " << static_cast<int>(dir);
  }
}

}  // namespace lite
}  // namespace paddle
