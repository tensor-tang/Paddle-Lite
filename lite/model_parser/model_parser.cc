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

#include "lite/model_parser/model_parser.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include "lite/core/scope.h"
#include "lite/core/tensor.h"
#include "lite/core/variable.h"
#include "lite/model_parser/desc_apis.h"
#include "lite/model_parser/naive_buffer/combined_params_desc.h"
#include "lite/model_parser/naive_buffer/param_desc.h"
#include "lite/model_parser/naive_buffer/program_desc.h"
#include "lite/model_parser/naive_buffer/var_desc.h"
#ifndef LITE_ON_TINY_PUBLISH
#include "lite/model_parser/pb/program_desc.h"
#include "lite/model_parser/pb/var_desc.h"
#endif
#include "lite/utils/io.h"

#ifdef LITE_WITH_NPU
#include "lite/backends/npu/npu_helper.h"
#endif

namespace paddle {
namespace lite {

#ifndef LITE_ON_TINY_PUBLISH
int SizeOfType(framework::proto::VarType::Type type) {
  using Type = framework::proto::VarType::Type;
  switch (static_cast<int>(type)) {
#define DO(desc, type)            \
  case Type::VarType_Type_##desc: \
    return sizeof(type);
    DO(BOOL, bool);
    DO(FP16, float);
    DO(FP32, float);
    DO(INT8, int8_t);
    DO(INT32, int);
    DO(INT64, int64_t);
#undef DO
    default:
      LOG(FATAL) << "unknown data type " << type;
  }
  return -1;
}

void TensorFromStream(std::istream &is, lite::Tensor *tensor) {
  using Type = framework::proto::VarType::Type;
  uint32_t version;
  is.read(reinterpret_cast<char *>(&version), sizeof(version));
  CHECK_EQ(version, 0U) << "Only version 0 is supported";
  // read tensor desc
  framework::proto::VarType::TensorDesc desc;
  {
    // int32_t size
    // proto buffer
    int32_t size;
    is.read(reinterpret_cast<char *>(&size), sizeof(size));
    std::unique_ptr<char[]> buf(new char[size]);
    is.read(reinterpret_cast<char *>(buf.get()), size);
    CHECK(desc.ParseFromArray(buf.get(), size)) << "Cannot parse tensor desc";
  }

  // read tensor
  std::vector<int64_t> dims_vec;
  std::copy(
      desc.dims().begin(), desc.dims().end(), std::back_inserter(dims_vec));
  lite::DDim dims(dims_vec);
  tensor->Resize(dims);
  void *buf;
  size_t size = tensor->dims().production() * SizeOfType(desc.data_type());
  // alllocate memory
  switch (static_cast<int>(desc.data_type())) {
#define SET_TENSOR(desc, type, precision) \
  case Type::VarType_Type_##desc:         \
    buf = tensor->mutable_data<type>();   \
    tensor->set_precision(precision);     \
    break

    // SET_TENSOR(BOOL, bool, PRECISION(kBool));
    SET_TENSOR(FP32, float, PRECISION(kFloat));
    SET_TENSOR(INT8, int8_t, PRECISION(kInt8));
    SET_TENSOR(INT16, int16_t, PRECISION(kInt16));
    SET_TENSOR(INT32, int32_t, PRECISION(kInt32));
    SET_TENSOR(INT64, int64_t, PRECISION(kInt64));
#undef SET_TENSOR
    default:
      LOG(FATAL) << "unknown type " << desc.data_type();
  }
  tensor->set_persistable(true);

  is.read(static_cast<char *>(buf), size);
}

void LoadLoDTensor(std::istream &is, Variable *var) {
  auto *tensor = var->GetMutable<lite::Tensor>();
  uint32_t version{};
  is.read(reinterpret_cast<char *>(&version), sizeof(version));
  VLOG(3) << "model version " << version;

  // Load LoD information
  uint64_t lod_level{};
  is.read(reinterpret_cast<char *>(&lod_level), sizeof(lod_level));
  auto &lod = *tensor->mutable_lod();
  lod.resize(lod_level);
  for (uint64_t i = 0; i < lod_level; ++i) {
    uint64_t size;
    is.read(reinterpret_cast<char *>(&size), sizeof(size));
    std::vector<uint64_t> tmp(size / sizeof(uint64_t));
    is.read(reinterpret_cast<char *>(tmp.data()),
            static_cast<std::streamsize>(size));
    lod[i] = tmp;
  }

  TensorFromStream(is, tensor);
}

void ReadBinaryFile(const std::string &filename, std::string *contents) {
  std::ifstream fin(filename, std::ios::in | std::ios::binary);
  CHECK(fin.is_open()) << "Cannot open file: " << filename;
  fin.seekg(0, std::ios::end);
  auto size = fin.tellg();
  contents->clear();
  contents->resize(size);
  fin.seekg(0, std::ios::beg);
  fin.read(&(contents->at(0)), contents->size());
  fin.close();
}

std::unique_ptr<framework::proto::ProgramDesc> LoadProgram(
    const std::string &path, bool program_from_memory) {
  std::unique_ptr<framework::proto::ProgramDesc> main_program(
      new framework::proto::ProgramDesc);
  if (!program_from_memory) {
    std::string desc_str;
    ReadBinaryFile(path, &desc_str);
    main_program->ParseFromString(desc_str);
  } else {
    main_program->ParseFromString(path);
  }
  return main_program;
}

void LoadParams(const std::string &path) {}

// Load directly to CPU, and latter transfer to other devices.
void LoadParam(const std::string &path, Variable *out) {
  std::ifstream fin(path, std::ios::binary);
  CHECK(fin.is_open()) << "failed to open file " << path;
  LoadLoDTensor(fin, out);
}

bool IsPersistable(const cpp::VarDesc &var) {
  if (var.Persistable() && var.GetType() != VarDescAPI::Type::FEED_MINIBATCH &&
      var.GetType() != VarDescAPI::Type::FETCH_LIST &&
      var.GetType() != VarDescAPI::Type::RAW) {
    return true;
  }
  return false;
}

void LoadCombinedParamsPb(const std::string &path,
                          lite::Scope *scope,
                          const cpp::ProgramDesc &cpp_prog,
                          bool params_from_memory) {
  CHECK(scope);
  auto prog = cpp_prog;
  auto &main_block_desc = *prog.GetBlock<cpp::BlockDesc>(0);

  // Get vars
  std::vector<std::string> paramlist;
  for (size_t i = 0; i < main_block_desc.VarsSize(); ++i) {
    auto &var = *main_block_desc.GetVar<cpp::VarDesc>(i);
    if (!IsPersistable(var)) continue;
    paramlist.push_back(var.Name());
  }
  std::sort(paramlist.begin(), paramlist.end());

  // Load vars
  auto load_var_func = [&](std::istream &is) {
    for (size_t i = 0; i < paramlist.size(); ++i) {
      auto *var = scope->Var(paramlist[i]);
      // Error checking
      CHECK(static_cast<bool>(is))
          << "There is a problem with loading model parameters";
      LoadLoDTensor(is, var);
    }
    is.peek();
    CHECK(is.eof()) << "You are not allowed to load partial data via"
                    << " LoadCombinedParamsPb, use LoadParam instead.";
  };

  if (params_from_memory) {
    std::stringstream fin(path, std::ios::in | std::ios::binary);
    load_var_func(fin);
  } else {
    std::ifstream fin(path, std::ios::binary);
    CHECK(fin.is_open());
    load_var_func(fin);
  }
}

void LoadModelPb(const std::string &model_dir,
                 const std::string &model_file,
                 const std::string &param_file,
                 Scope *scope,
                 cpp::ProgramDesc *cpp_prog,
                 bool combined,
                 bool model_from_memory) {
  CHECK(cpp_prog);
  CHECK(scope);
  cpp_prog->ClearBlocks();

  // Load model
  VLOG(4) << "Start load model program...";
  std::string prog_path = model_dir + "/__model__";
  if (combined) {
    prog_path = model_file;
  }
  framework::proto::ProgramDesc pb_proto_prog =
      *LoadProgram(prog_path, model_from_memory);
  pb::ProgramDesc pb_prog(&pb_proto_prog);
  // Transform to cpp::ProgramDesc
  TransformProgramDescAnyToCpp(pb_prog, cpp_prog);

  // Load Params
  // NOTE: Only main block be used now.
  VLOG(4) << "Start load model params...";
  CHECK(!(!combined && model_from_memory))
      << "If you want use the model_from_memory,"
      << " you should load the combined model using cfg.set_model_buffer "
         "interface.";
  if (combined) {
    LoadCombinedParamsPb(param_file, scope, *cpp_prog, model_from_memory);
  } else {
    auto main_block = pb_proto_prog.blocks(0);
    for (auto &var : main_block.vars()) {
      if (var.name() == "feed" || var.name() == "fetch" || !var.persistable())
        continue;

      std::string file_path = model_dir + "/" + var.name();
      VLOG(4) << "reading weight " << var.name();

      std::ifstream file(file_path);
      switch (var.type().type()) {
        case framework::proto::VarType_Type_LOD_TENSOR:
          LoadLoDTensor(file, scope->Var(var.name()));
          break;
        default:
          CHECK(false) << "unknown weight type";
      }
    }
  }

#ifdef LITE_WITH_NPU
  auto main_block = pb_proto_prog.blocks(0);
  for (auto &op : main_block.ops()) {
    LOG(INFO) << "op type:" << op.type();
    if (op.type() != "graph_op") {
      continue;
    }
    auto xs = op.attrs();
    auto it = std::find_if(
        xs.begin(), xs.end(), [&](const framework::proto::OpDesc_Attr &x) {
          return x.name() == "model_name";
        });
    CHECK(it != xs.end());
    auto model_name = it->s();
    std::string file_path = model_dir + "/" + model_name;
    CHECK(npu::BuildNPUClient(file_path, model_name))
        << "NPU model load failed!";
  }
#endif
  VLOG(4) << "Load protobuf model in '" << model_dir << "'' successfully";
}

void SaveModelPb(const std::string &model_dir,
                 const Scope &exec_scope,
                 const cpp::ProgramDesc &cpp_prog,
                 bool combined) {
  MkDirRecur(model_dir);
  // Save program
  framework::proto::ProgramDesc pb_proto_prog;
  pb::ProgramDesc pb_prog(&pb_proto_prog);
  TransformProgramDescCppToAny(cpp_prog, &pb_prog);

  std::string prog_path = model_dir + "/__model__";
  if (combined) {
    prog_path = model_dir + "/model";
  }
  std::ofstream model_ostream(prog_path, std::ios_base::binary);
  CHECK(model_ostream.is_open());
  const std::string pb_str = pb_proto_prog.SerializeAsString();
  model_ostream.write(pb_str.c_str(), pb_str.size());
  model_ostream.close();

  // Save Params
  // NOTE: Only main block be used now.
  if (combined) {
    const std::string combined_params_path = model_dir + "/params";
    SaveCombinedParamsPb(combined_params_path, exec_scope, cpp_prog);
  } else {
    for (auto &item : pb_proto_prog.blocks(0).vars()) {
      if (item.name() == "feed" || item.name() == "fetch" ||
          !item.persistable())
        continue;
      const std::string path = model_dir + "/" + item.name();
      std::ofstream var_ostream(path, std::ios::binary);
      CHECK(var_ostream.is_open());
      SerializeTensor(var_ostream, exec_scope, item.name());
      var_ostream.close();
    }
  }
  VLOG(4) << "Save protobuf model in '" << model_dir << "'' successfully";
}

void SaveCombinedParamsPb(const std::string &path,
                          const lite::Scope &exec_scope,
                          const cpp::ProgramDesc &cpp_prog) {
  auto prog = cpp_prog;
  auto &main_block_desc = *prog.GetBlock<cpp::BlockDesc>(0);

  // Get vars
  std::vector<std::string> paramlist;
  for (size_t i = 0; i < main_block_desc.VarsSize(); ++i) {
    auto &var = *main_block_desc.GetVar<cpp::VarDesc>(i);
    if (!IsPersistable(var)) continue;
    paramlist.push_back(var.Name());
  }
  std::sort(paramlist.begin(), paramlist.end());

  // Load vars
  std::ofstream file(path);
  CHECK(file.is_open());
  for (size_t i = 0; i < paramlist.size(); ++i) {
    SerializeTensor(file, exec_scope, paramlist[i]);
  }
  file.close();
}

void TensorToStream(std::ostream &os, const lite::Tensor &tensor) {
  // the 1st field, uint32_t version
  constexpr uint32_t version = 0;
  os.write(reinterpret_cast<const char *>(&version), sizeof(version));

  {
    uint64_t size = tensor.lod().size();
    // the 2st field, LoD information
    // uint64_t lod_level
    // uint64_t lod_level_1 size in byte.
    // int*     lod_level_1 data
    // ...
    os.write(reinterpret_cast<const char *>(&size), sizeof(size));

    for (auto &each : tensor.lod()) {
      size = each.size() * sizeof(each.front());
      os.write(reinterpret_cast<const char *>(&size), sizeof(size));
      os.write(reinterpret_cast<const char *>(each.data()),
               static_cast<std::streamsize>(size));
    }
  }

  // There are two version fields in a LoDTensor.
  os.write(reinterpret_cast<const char *>(&version), sizeof(version));

  {  // the 2nd field, tensor description
    // int32_t  size
    // void*    protobuf message
    framework::proto::VarType::TensorDesc desc;
    // TODO(Superjomn) support other data types.
    switch (tensor.precision()) {
#define SET_DATA_TYPE(precision, type_desc) \
  case precision:                           \
    desc.set_data_type(type_desc);          \
    break

      SET_DATA_TYPE(PRECISION(kFloat), framework::proto::VarType_Type_FP32);
      SET_DATA_TYPE(PRECISION(kInt8), framework::proto::VarType_Type_INT8);
      SET_DATA_TYPE(PRECISION(kInt16), framework::proto::VarType_Type_INT16);
      SET_DATA_TYPE(PRECISION(kInt32), framework::proto::VarType_Type_INT32);
      SET_DATA_TYPE(PRECISION(kInt64), framework::proto::VarType_Type_INT64);
#undef SET_DATA_TYPE
      default:
        LOG(FATAL) << "unknown precision type: "
                   << PrecisionToStr(tensor.precision());
    }
    auto dims = tensor.dims();
    auto *pb_dims = desc.mutable_dims();
    pb_dims->Resize(static_cast<int>(dims.size()), 0);
    auto dims_vec = dims.Vectorize();
    std::copy(dims_vec.begin(), dims_vec.end(), pb_dims->begin());
    int32_t size = desc.ByteSize();
    os.write(reinterpret_cast<const char *>(&size), sizeof(size));
    auto out = desc.SerializeAsString();
    os.write(out.data(), size);
  }
  {  // the 3rd field, tensor data
    uint64_t size = tensor.memory_size();
    CHECK_LT(size, std::numeric_limits<std::streamsize>::max())
        << "Index overflow when writing tensor";

#ifdef LITE_WITH_CUDA
    if (tensor.target() == TARGET(kCUDA)) {
      std::unique_ptr<char> tmp_buffer(new char[size]);
      TargetWrapperCuda::MemcpySync(tmp_buffer.get(),
                                    tensor.data<float>(),
                                    tensor.data_size(),
                                    IoDirection::DtoH);
      os.write(static_cast<const char *>(tmp_buffer.get()),
               static_cast<std::streamsize>(size));
    } else  // NOLINT
#endif      // LITE_WITH_CUDA
    {
      os.write(static_cast<const char *>(tensor.data<void>()),
               static_cast<std::streamsize>(size));
    }
  }
}

void SerializeTensor(std::ostream &os,
                     const lite::Scope &scope,
                     const std::string &var_name) {
  // Store all the persistable vars.
  auto *var = scope.FindVar(var_name);
  const auto &tensor = var->Get<lite::Tensor>();
  TensorToStream(os, tensor);
}

/// For navie buffer
void SetParamInfoNaive(naive_buffer::ParamDesc *param_desc,
                       const lite::Scope &scope,
                       const std::string &var_name) {
  CHECK(param_desc);
  auto &desc = *param_desc;

  // the 1st field, uint32_t version
  constexpr uint32_t version = 0;

  auto *var = scope.FindVar(var_name);
  const auto &tensor = var->Get<lite::Tensor>();

  desc.SetName(var_name);

  desc.SetModelVersion(version);
  desc.SetTensorVersion(version);

  desc.SetLoDLevel(tensor.lod().size());
  desc.SetLoD(tensor.lod());

  // TODO(sangoly): support other data types.
  switch (tensor.precision()) {
#define SET_DATA_TYPE(precision, type_desc) \
  case precision:                           \
    desc.SetDataType(type_desc);            \
    break

    SET_DATA_TYPE(PRECISION(kFloat), VarDescAPI::VarDataType::FP32);
    SET_DATA_TYPE(PRECISION(kInt8), VarDescAPI::VarDataType::INT8);
    SET_DATA_TYPE(PRECISION(kInt16), VarDescAPI::VarDataType::INT16);
    SET_DATA_TYPE(PRECISION(kInt32), VarDescAPI::VarDataType::INT32);
    SET_DATA_TYPE(PRECISION(kInt64), VarDescAPI::VarDataType::INT64);
#undef SET_DATA_TYPE
    default:
      LOG(FATAL) << "unknown precision type: "
                 << PrecisionToStr(tensor.precision());
  }
  desc.SetDim(tensor.dims().Vectorize());
  uint64_t size = tensor.memory_size();
  CHECK_LT(size, std::numeric_limits<std::streamsize>::max())
      << "Index overflow when writing tensor";

#ifdef LITE_WITH_CUDA
  if (tensor.target() == TARGET(kCUDA)) {
    switch (tensor.precision()) {
#define DO(precision, type)                                         \
  case precision:                                                   \
    std::unique_ptr<type> tmp_buffer(new type[tensor.data_size()]); \
    TargetWrapperCuda::MemcpySync(tmp_buffer.get(),                 \
                                  tensor.data<type>(),              \
                                  tensor.data_size(),               \
                                  IoDirection::DtoH);               \
    desc.SetData<type>(tmp_buffer.get(), tensor.data_size());       \
    break
      DO(PRECISION(kFloat), float);
      DO(PRECISION(kInt8), int8_t);
      DO(PRECISION(kInt16), int16_t);
      DO(PRECISION(kInt32), int32_t);
      DO(PRECISION(kInt64), int64_t);
#undef DO
      default:
        LOG(FATAL) << "unknown precision type: "
                   << PrecisionToStr(tensor.precision());
    }
  } else  // NOLINT
#endif    // LITE_WITH_CUDA
  {
    switch (tensor.precision()) {
#define DO(precision, type)                                      \
  case precision:                                                \
    desc.SetData<type>(tensor.data<type>(), tensor.data_size()); \
    break
      DO(PRECISION(kFloat), float);
      DO(PRECISION(kInt8), int8_t);
      DO(PRECISION(kInt16), int16_t);
      DO(PRECISION(kInt32), int32_t);
      DO(PRECISION(kInt64), int64_t);
#undef DO
      default:
        LOG(FATAL) << "unknown precision type: "
                   << PrecisionToStr(tensor.precision());
    }
  }
}

void SaveParamNaive(const std::string &path,
                    const lite::Scope &scope,
                    const std::string &var_name) {
  naive_buffer::BinaryTable table;
  naive_buffer::proto::ParamDesc pt_desc(&table);
  naive_buffer::ParamDesc desc(&pt_desc);

  SetParamInfoNaive(&desc, scope, var_name);

  // Save param
  pt_desc.Save();
  table.SaveToFile(path);
}

void SaveCombinedParamsNaive(const std::string &path,
                             const lite::Scope &exec_scope,
                             const cpp::ProgramDesc &cpp_prog) {
  naive_buffer::BinaryTable table;
  naive_buffer::proto::CombinedParamsDesc pt_desc(&table);
  naive_buffer::CombinedParamsDesc desc(&pt_desc);

  auto prog = cpp_prog;
  auto &main_block_desc = *prog.GetBlock<cpp::BlockDesc>(0);
  for (size_t i = 0; i < main_block_desc.VarsSize(); ++i) {
    auto &var = *main_block_desc.GetVar<cpp::VarDesc>(i);
    if (var.Name() == "feed" || var.Name() == "fetch" || !var.Persistable())
      continue;
    naive_buffer::ParamDesc param_desc(desc.AddParam());
    SetParamInfoNaive(&param_desc, exec_scope, var.Name());
  }

  pt_desc.Save();
  table.SaveToFile(path);
}

void SaveModelNaive(const std::string &model_dir,
                    const Scope &exec_scope,
                    const cpp::ProgramDesc &cpp_prog,
                    bool combined) {
  MkDirRecur(model_dir);
  // Save program
  const std::string prog_path = model_dir + "/__model__.nb";
  naive_buffer::BinaryTable table;
  naive_buffer::proto::ProgramDesc nb_proto_prog(&table);
  naive_buffer::ProgramDesc nb_prog(&nb_proto_prog);
  TransformProgramDescCppToAny(cpp_prog, &nb_prog);
  nb_proto_prog.Save();
  table.SaveToFile(prog_path);

  // Save Params
  // NOTE: Only main block be used now.
  if (combined) {
    const std::string combined_params_path = model_dir + "/param.nb";
    SaveCombinedParamsNaive(combined_params_path, exec_scope, cpp_prog);
  } else {
    auto prog = cpp_prog;
    auto &main_block_desc = *prog.GetBlock<cpp::BlockDesc>(0);
    for (size_t i = 0; i < main_block_desc.VarsSize(); ++i) {
      auto &var = *main_block_desc.GetVar<cpp::VarDesc>(i);
      if (var.Name() == "feed" || var.Name() == "fetch" || !var.Persistable())
        continue;
      const std::string path = model_dir + "/" + var.Name() + ".nb";
      SaveParamNaive(path, exec_scope, var.Name());
    }
  }
  VLOG(4) << "Save naive buffer model in '" << model_dir << "'' successfully";
}
#endif

template <typename T>
void SetTensorDataNaive(T *out, size_t size, const std::vector<T> &src) {
  CHECK(out);
  CHECK(size == src.size());
  for (size_t i = 0; i < size; ++i) {
    out[i] = src[i];
  }
}

void GetParamInfoNaive(const naive_buffer::ParamDesc &desc,
                       lite::Scope *scope,
                       const std::string &name) {
  CHECK(scope);
  CHECK_EQ(desc.Name(), name)
      << "Var name not equal: ParamDesc.name=" << desc.Name()
      << "vs filename=" << name;

  auto *tensor = scope->Var(name)->GetMutable<lite::Tensor>();

  VLOG(3) << "model version " << desc.ModelVersion();
  CHECK_EQ(desc.TensorVersion(), 0U) << "Only version 0 is supported";

  // Load LoD info
  auto *tgt_lod = tensor->mutable_lod();
  auto desc_lod = desc.LoD();
  tgt_lod->assign(desc_lod.begin(), desc_lod.end());

  // Load Dim info
  tensor->Resize(lite::DDim(desc.Dim()));

  // Load data
  switch (desc.GetDataType()) {
#define SET_TENSOR(data_type__, T, precision)                            \
  case VarDescAPI::VarDataType::data_type__:                             \
    SetTensorDataNaive<T>(                                               \
        tensor->mutable_data<T>(), tensor->data_size(), desc.Data<T>()); \
    tensor->set_precision(precision);                                    \
    break

    // SET_TENSOR(BOOL, bool, PRECISION(kBool));
    SET_TENSOR(FP32, float, PRECISION(kFloat));
    SET_TENSOR(INT8, int8_t, PRECISION(kInt8));
    SET_TENSOR(INT16, int16_t, PRECISION(kInt16));
    SET_TENSOR(INT32, int32_t, PRECISION(kInt32));
    SET_TENSOR(INT64, int64_t, PRECISION(kInt64));
#undef SET_TENSOR
    default:
      LOG(FATAL) << "unknown type";
  }
  tensor->set_persistable(true);
}

void LoadParamNaive(const std::string &path,
                    lite::Scope *scope,
                    const std::string &name) {
  // Load param
  naive_buffer::BinaryTable table;
  table.LoadFromFile(path);
  naive_buffer::proto::ParamDesc pt_desc(&table);
  pt_desc.Load();
  naive_buffer::ParamDesc desc(&pt_desc);
  GetParamInfoNaive(desc, scope, name);
}

void LoadCombinedParamsNaive(const std::string &path,
                             lite::Scope *scope,
                             const cpp::ProgramDesc &cpp_prog,
                             bool params_from_memory) {
  naive_buffer::BinaryTable table;
  if (params_from_memory) {
    table.LoadFromMemory(path.c_str(), path.length());
  } else {
    table.LoadFromFile(path);
  }
  naive_buffer::proto::CombinedParamsDesc pt_desc(&table);
  pt_desc.Load();
  naive_buffer::CombinedParamsDesc desc(&pt_desc);

  std::set<std::string> param_names;
  for (size_t i = 0; i < desc.ParamsSize(); ++i) {
    naive_buffer::ParamDesc param_desc(desc.GetParam(i));
    GetParamInfoNaive(param_desc, scope, param_desc.Name());
    param_names.insert(param_desc.Name());
  }

  // Check all params loaded
  auto prog = cpp_prog;
  auto &main_block_desc = *prog.GetBlock<cpp::BlockDesc>(0);
  for (size_t i = 0; i < main_block_desc.VarsSize(); ++i) {
    auto &var = *main_block_desc.GetVar<cpp::VarDesc>(i);
    if (var.Name() == "feed" || var.Name() == "fetch" || !var.Persistable())
      continue;
    CHECK(param_names.count(var.Name())) << "Persistable var[" << var.Name()
                                         << "] not found";
  }
}

void LoadModelNaive(const std::string &model_dir,
                    Scope *scope,
                    cpp::ProgramDesc *cpp_prog,
                    bool combined) {
  CHECK(cpp_prog);
  CHECK(scope);
  cpp_prog->ClearBlocks();

  // Load model
  const std::string prog_path = model_dir + "/__model__.nb";
  naive_buffer::BinaryTable table;
  table.LoadFromFile(prog_path);
  naive_buffer::proto::ProgramDesc nb_proto_prog(&table);
  nb_proto_prog.Load();
  naive_buffer::ProgramDesc nb_prog(&nb_proto_prog);

  // Transform to cpp::ProgramDesc
  TransformProgramDescAnyToCpp(nb_prog, cpp_prog);

  // Load Params
  // NOTE: Only main block be used now.
  if (combined) {
    const std::string combined_params_path = model_dir + "/param.nb";
    LoadCombinedParamsNaive(combined_params_path, scope, *cpp_prog, false);
  } else {
    auto &prog = *cpp_prog;
    auto &main_block_desc = *prog.GetBlock<cpp::BlockDesc>(0);
    for (size_t i = 0; i < main_block_desc.VarsSize(); ++i) {
      auto &var = *main_block_desc.GetVar<cpp::VarDesc>(i);
      if (var.Name() == "feed" || var.Name() == "fetch" || !var.Persistable())
        continue;

      std::string file_path = model_dir + "/" + var.Name() + ".nb";
      VLOG(4) << "reading weight " << var.Name();

      switch (var.GetType()) {
        case VarDescAPI::Type::LOD_TENSOR:
          LoadParamNaive(file_path, scope, var.Name());
          break;
        default:
          CHECK(false) << "unknown weight type";
      }
    }
  }

#ifdef LITE_WITH_NPU
  auto &prog = *cpp_prog;
  auto &main_block_desc = *prog.GetBlock<cpp::BlockDesc>(0);
  for (size_t i = 0; i < main_block_desc.OpsSize(); ++i) {
    auto &op = *main_block_desc.GetOp<cpp::OpDesc>(i);
    if (op.Type() != "graph_op") {
      continue;
    }
    auto model_name = op.GetAttr<std::string>("model_name");
    std::string file_path = model_dir + "/" + model_name;
    CHECK(npu::BuildNPUClient(file_path, model_name))
        << "NPU model load failed!";
  }
#endif

  VLOG(4) << "Load naive buffer model in '" << model_dir << "' successfully";
}

void LoadModelNaiveFromMemory(const std::string &model_buffer,
                              const std::string &param_buffer,
                              Scope *scope,
                              cpp::ProgramDesc *cpp_prog) {
  CHECK(cpp_prog);
  CHECK(scope);
  cpp_prog->ClearBlocks();

  // Load model

  std::string prog_path = model_buffer;

  naive_buffer::BinaryTable table;
  table.LoadFromMemory(prog_path.c_str(), prog_path.length());

  naive_buffer::proto::ProgramDesc nb_proto_prog(&table);
  nb_proto_prog.Load();
  naive_buffer::ProgramDesc nb_prog(&nb_proto_prog);

  // Transform to cpp::ProgramDesc
  TransformProgramDescAnyToCpp(nb_prog, cpp_prog);

  // Load Params
  // NOTE: Only main block be used now.
  // only combined Params are supported in Loading Model from memory
  std::string combined_params_path = param_buffer;
  LoadCombinedParamsNaive(combined_params_path, scope, *cpp_prog, true);

#ifdef LITE_WITH_NPU
  LOG(FATAL) << "load from memory is not supported by NPU";
#endif

  VLOG(4) << "Load model from naive buffer memory successfully";
}

}  // namespace lite
}  // namespace paddle
