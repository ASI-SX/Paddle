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

#include "paddle/fluid/imperative/layer.h"
#include <algorithm>
#include <queue>
#include <utility>
#include "paddle/fluid/framework/framework.pb.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/framework/variable_helper.h"
#include "paddle/fluid/imperative/prepared_operator.h"
#include "paddle/fluid/operators/math/math_function.h"
#include "paddle/fluid/platform/device_context.h"
#include "paddle/fluid/platform/enforce.h"
#include "paddle/fluid/platform/profiler.h"

namespace paddle {
namespace imperative {

using framework::Variable;
void ThreadSafeNameSet::Insert(const std::string& name) {
  std::lock_guard<std::mutex> guard(mtx_);
  set_.insert(name);
}

void ThreadSafeNameSet::Remove(const std::string& name) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto iter = set_.find(name);
  PADDLE_ENFORCE_EQ(iter != set_.end(), true, "%s does not exist", name);
  set_.erase(iter);
}

std::vector<std::string> ThreadSafeNameSet::Names() const {
  std::lock_guard<std::mutex> guard(mtx_);
  return std::vector<std::string>(set_.begin(), set_.end());
}

ThreadSafeNameSet VarBase::name_set_;

std::vector<std::string> VarBase::AliveVarNames() { return name_set_.Names(); }

static framework::VariableNameMap CreateVarNameMap(
    const framework::OpInfo& op_info, const std::string& op_type,
    const NameVarBaseMap& varbase_map, bool is_input) {
  if (op_info.proto_ == nullptr) {
    framework::VariableNameMap result;

    for (auto& it : varbase_map) {
      auto& var_vector = it.second;
      std::vector<std::string> args;
      args.reserve(var_vector.size());
      for (auto& var_base : var_vector) {
        args.emplace_back(var_base->Name());
      }
      result[it.first] = std::move(args);
    }
    return result;
  }

  framework::VariableNameMap result;

  for (auto& var :
       is_input ? op_info.Proto().inputs() : op_info.Proto().outputs()) {
    auto it = varbase_map.find(var.name());
    if (it == varbase_map.end()) {
      PADDLE_ENFORCE_EQ(
          var.dispensable(), true,
          "Var: %s not dispensable and there are no such var in inputs",
          var.name());
      result[var.name()] = {};
    } else {
      auto& var_vector = it->second;
      std::vector<std::string> args;
      args.reserve(var_vector.size());
      for (auto& var_base : var_vector) {
        args.emplace_back(var_base->Name());
      }
      result[var.name()] = std::move(args);
    }
  }
  return result;
}

static framework::RuntimeContext PrepareRuntimeContext(
    const NameVarBaseMap& ins, const NameVarBaseMap& outs) {
  framework::VariableValueMap inputs, outputs;
  for (auto& in_pair : ins) {
    auto& in_ctx = inputs[in_pair.first];
    in_ctx.reserve(in_pair.second.size());
    for (auto& in_var : in_pair.second) {
      in_ctx.emplace_back(in_var->MutableVar());
    }
  }

  for (auto& out_pair : outs) {
    auto& out_ctx = outputs[out_pair.first];
    out_ctx.reserve(out_pair.second.size());
    for (auto& out_var : out_pair.second) {
      out_ctx.emplace_back(out_var->MutableVar());
    }
  }
  return framework::RuntimeContext(std::move(inputs), std::move(outputs));
}

template <typename VarType>
static std::string DebugString(
    const std::string& name,
    const std::vector<std::shared_ptr<VarType>>& vars) {
  std::stringstream ss;
  ss << name << "{";

  for (size_t i = 0; i < vars.size(); ++i) {
    if (i > 0) ss << ", ";

    if (vars[i] == nullptr) {
      ss << "NULL";
      continue;
    }
    ss << vars[i]->Name() << "[";
    const framework::Variable& var = vars[i]->Var();
    if (!var.IsInitialized()) {
      ss << "NOT_INITED_VAR";
    } else if (var.IsType<framework::LoDTensor>()) {
      auto& tensor = var.Get<framework::LoDTensor>();
      ss << "LoDTensor<";
      if (tensor.IsInitialized()) {
        ss << framework::DataTypeToString(tensor.type()) << ", ";
        ss << tensor.place() << ", ";
        ss << "(" << tensor.dims() << ")";
      } else {
        ss << "NOT_INITED";
      }
      ss << ">";
    } else if (var.IsType<framework::SelectedRows>()) {
      ss << "SelectedRows<";
      auto& selected_rows = var.Get<framework::SelectedRows>();
      auto& tensor = selected_rows.value();
      auto& rows = selected_rows.rows();
      if (tensor.IsInitialized()) {
        ss << framework::DataTypeToString(tensor.type()) << ", ";
        ss << tensor.place() << ", ";
        ss << "height(" << selected_rows.height() << "), rows(";
        std::for_each(rows.cbegin(), rows.cend(),
                      [&ss](const int64_t r) { ss << r << " "; });
        ss << "), dims(" << tensor.dims() << ")";
      } else {
        ss << "NOT_INITED";
      }
      ss << ">";
    } else {
      ss << "UNRESOLVED_TYPE";
    }
    ss << "]";
  }

  ss << "}";
  return ss.str();
}

template <typename VarType>
static std::string LayerDebugStringImpl(const std::string& op_type,
                                        const NameVarMap<VarType>& ins,
                                        const NameVarMap<VarType>& outs) {
  std::stringstream ss;
  ss << "Op(" << op_type << "): ";

  ss << "Inputs: ";

  size_t i = 0;
  for (auto& pair : ins) {
    if (i > 0) ss << ", ";
    ss << DebugString(pair.first, pair.second);
    ++i;
  }

  ss << ",   Outputs: ";
  i = 0;
  for (auto& pair : outs) {
    if (i > 0) ss << ", ";
    ss << DebugString(pair.first, pair.second);
    ++i;
  }
  return ss.str();
}

std::string LayerDebugString(const std::string& op_type,
                             const NameVarMap<VarBase>& ins,
                             const NameVarMap<VarBase>& outs) {
  return LayerDebugStringImpl<VarBase>(op_type, ins, outs);
}

std::string LayerDebugString(const std::string& op_type,
                             const NameVarMap<VariableWrapper>& ins,
                             const NameVarMap<VariableWrapper>& outs) {
  return LayerDebugStringImpl<VariableWrapper>(op_type, ins, outs);
}

void VarBase::ClearGradient() {
  if (grad_var_) {
    if (grad_var_->Var().IsType<framework::SelectedRows>()) {
      auto* grad_t =
          grad_var_->MutableVar()->GetMutable<framework::SelectedRows>();
      if (grad_t->mutable_value()->IsInitialized()) {
        grad_t->mutable_rows()->clear();
        grad_t->mutable_value()->clear();
      }
    } else {
      auto* grad_t =
          grad_var_->MutableVar()->GetMutable<framework::LoDTensor>();
      if (grad_t->IsInitialized()) {
        auto* dev_ctx =
            platform::DeviceContextPool::Instance().Get(grad_t->place());
        operators::math::set_constant(*dev_ctx, grad_t, 0.0);
      }
    }
  }
}

std::shared_ptr<VarBase> VarBase::NewVarBase(const platform::Place& dst_place,
                                             const bool blocking) const {
  PADDLE_ENFORCE_EQ(
      Var().IsInitialized() && (Var().IsType<framework::LoDTensor>() ||
                                Var().IsType<framework::SelectedRows>()),
      true, platform::errors::InvalidArgument(
                "Variable is not initialized or Variable's type is not "
                "LoDTensor or SelectedRows when getting numpy tensor"));
  if (Var().IsType<framework::LoDTensor>()) {
    auto& src_tensor = Var().Get<framework::LoDTensor>();

    // TODO(Jiabin): change this after move unique_name generator to CXX
    auto new_var = std::make_shared<VarBase>(
        true, Name() + std::to_string(copied_counter_++));

    auto* dst_tensor =
        new_var->MutableVar()->GetMutable<framework::LoDTensor>();
    dst_tensor->set_lod(src_tensor.lod());
    new_var->SetPersistable(Persistable());
    new_var->SetDataType(DataType());
    new_var->SetType(Type());
    framework::TensorCopy(src_tensor, dst_place, dst_tensor);
    if (blocking) {
      platform::DeviceContextPool::Instance().Get(dst_place)->Wait();
      auto src_place = src_tensor.place();
      if (!(src_place == dst_place)) {
        platform::DeviceContextPool::Instance().Get(src_place)->Wait();
      }
    }

    if (platform::is_gpu_place(dst_place)) {
      VLOG(3) << "copy tensor " << Name() << " from gpu";
    }
    return new_var;
  } else {
    auto& src_selected_rows = Var().Get<framework::SelectedRows>();
    auto new_var = std::make_shared<VarBase>(
        false, "Itmp" + std::to_string(copied_counter_++));
    new_var->SetType(framework::proto::VarType::SELECTED_ROWS);
    auto* dst_selected_rows =
        new_var->MutableVar()->GetMutable<framework::SelectedRows>();

    framework::TensorCopy(src_selected_rows.value(), dst_place,
                          dst_selected_rows->mutable_value());
    if (blocking) {
      platform::DeviceContextPool::Instance().Get(dst_place)->Wait();
      auto src_place = src_selected_rows.place();
      if (!(src_place == dst_place)) {
        platform::DeviceContextPool::Instance().Get(src_place)->Wait();
      }
    }
    dst_selected_rows->set_height(src_selected_rows.height());
    dst_selected_rows->set_rows(src_selected_rows.rows());
    if (platform::is_gpu_place(dst_place)) {
      VLOG(3) << "copy selected rows " << Name() << " from gpu";
    }
    return new_var;
  }
}

void OpBase::SetType(const std::string& type) {
  op_ = framework::OpRegistry::CreateOp(type, {}, {}, {}, false);
}

void OpBase::ClearBackwardTrace() {
  grad_pending_ops_.clear();
  allow_empty_vars_.clear();
  ins_.clear();
  outs_.clear();
}

template <typename VarType>
static void OpBaseRunImpl(const framework::OperatorBase& op,
                          const NameVarMap<VarType>& ins,
                          const NameVarMap<VarType>& outs,
                          const framework::AttributeMap& attrs,
                          const platform::Place& place) {
  auto* op_kernel = dynamic_cast<const framework::OperatorWithKernel*>(&op);
  PADDLE_ENFORCE_NOT_NULL(op_kernel, "only support op with kernel");
  auto& info = op.Info();
  if (info.infer_var_type_) {
    RuntimeInferVarTypeContext<VarType> infer_var_type_ctx(ins, &outs, attrs);
    info.infer_var_type_(&infer_var_type_ctx);
  }

  // Initialize output var type
  for (auto& var_pair : outs) {
    for (auto& var : var_pair.second) {
      InitializeVariable(var->MutableVar(), var->Type());
    }
  }

  // VLOG(3) << "Running Op " << op.Type();
  VLOG(5) << LayerDebugString(op.Type(), ins, outs);
  auto prepared_op = PreparedOp::Prepare(ins, outs, *op_kernel, place, attrs);

  prepared_op.Run(ins, outs, attrs);

  VLOG(4) << LayerDebugString(op.Type(), ins, outs);
}

void OpBase::Run(const framework::OperatorBase& op,
                 const NameVarMap<VarBase>& ins,
                 const NameVarMap<VarBase>& outs,
                 const framework::AttributeMap& attrs,
                 const platform::Place& place) {
  OpBaseRunImpl<VarBase>(op, ins, outs, attrs, place);
}

void OpBase::Run(const framework::OperatorBase& op,
                 const NameVarMap<VariableWrapper>& ins,
                 const NameVarMap<VariableWrapper>& outs,
                 const framework::AttributeMap& attrs,
                 const platform::Place& place) {
  OpBaseRunImpl<VariableWrapper>(op, ins, outs, attrs, place);
}

}  // namespace imperative
}  // namespace paddle
