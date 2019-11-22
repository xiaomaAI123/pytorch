#include <torch/csrc/autograd/variable.h>

#include <torch/csrc/autograd/autograd.h>
#include <torch/csrc/autograd/edge.h>
#include <torch/csrc/autograd/engine.h>
#include <torch/csrc/autograd/function.h>
#include <torch/csrc/autograd/functions/accumulate_grad.h>
#include <torch/csrc/autograd/functions/tensor.h>
#include <torch/csrc/autograd/generated/Functions.h>

#include <ATen/core/VariableHooksInterface.h>

#include <ATen/ATen.h>
#include <c10/util/Exception.h>

#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <typeinfo>

namespace torch {
namespace autograd {


DifferentiableViewMeta::DifferentiableViewMeta(at::TensorImpl* self_impl, Variable base)
    : AutogradMeta(self_impl, false) {
  base_ = std::move(base);
  TORCH_CHECK(base_.defined(), "base is undefined");
  if (base_.is_view()) {
    base_ = base_.base();
  }
  is_view_ = true;
  self_impl->set_version_counter(impl::version_counter(base_));
  attr_version = self_impl->version_counter().current_version();
}

DifferentiableViewMeta::~DifferentiableViewMeta() {
  base_.reset();
}

namespace {

at::Tensor singleton_undefined_tensor;

struct ConcreteAutogradMetaFactory : public c10::impl::AutogradMetaFactory {
  std::unique_ptr<c10::AutogradMetaInterface> make() const override {
    return c10::guts::make_unique<AutogradMeta>();
  }
  const at::Tensor& undefined_tensor() const override {
    return singleton_undefined_tensor;
  }
};

ConcreteAutogradMetaFactory meta_factory;

static c10::impl::AutogradMetaFactoryRegisterer meta_factory_registerer(&meta_factory);

}

namespace impl {

  AutogradMeta* materialize_autograd_meta(const Variable& self) {
    TORCH_CHECK(self.defined(), "cannot call materialize_autograd_meta() on undefined tensor");
    auto p = self.unsafeGetTensorImpl();
    if (!p->autograd_meta()) {
      p->set_autograd_meta(c10::guts::make_unique<AutogradMeta>());
    }
    return get_autograd_meta(self);
  }

  void rebase_history(const Variable& self, Edge gradient_edge) {
    AT_ASSERT(gradient_edge.function != nullptr);
    if (self.is_view()) {
      // NB: is_view() ==> get_autograd_meta()
      auto diff_view_meta = static_cast<DifferentiableViewMeta*>(get_autograd_meta(self));
      AT_ASSERT(gradient_edge.input_nr == 0);
      AT_ASSERT(gradient_edge.function);
      TORCH_CHECK(
          gradient_edge.function->num_inputs() == 1,
          "Functions which modify views in-place must return a single Variable");
      diff_view_meta->output_nr_ = gradient_edge.input_nr;
      auto copy_slices = std::make_shared<CopySlices>(
          diff_view_meta->base_, at::TensorGeometry(self), std::move(gradient_edge.function));
      set_gradient_edge(diff_view_meta->base_, {std::move(copy_slices), 0});
      self.grad_fn(); // trigger an update to the view's grad_fn
    } else {
      set_gradient_edge(self, std::move(gradient_edge));
    }
  }

  // yf225 TODO: we should store cpp hooks in a dict, not a vector!
  void create_cpp_hook(const Variable& self) {
    auto &map = materialize_autograd_meta(self)->cpp_hooks_map;
    map.reset(new hooks_map());
    std::unique_ptr<FunctionPreHook> hook_ptr(new CppFunctionPreHook(map, self.output_nr()));
    clear_hooks(self);
    add_hook(self, std::make_shared<CppFunctionPreHook>(map, 0));
    auto fn = self.grad_fn();
    if (fn) {
      fn->add_pre_hook(std::move(hook_ptr));
    }
  }

  void set_grad_accumulator(const Variable& self,
      std::weak_ptr<Node> grad_accumulator) {
    materialize_autograd_meta(self)->grad_accumulator_ = std::move(grad_accumulator);
  }

  std::shared_ptr<Node> try_get_grad_accumulator(const Variable& self) {
    if (get_autograd_meta(self)) {
      return get_autograd_meta(self)->grad_accumulator_.lock();
    } else {
      return nullptr;
    }
  }

  std::shared_ptr<Node> grad_accumulator(const Variable& self) {
    auto autograd_meta = get_autograd_meta(self);
    if (!autograd_meta) {
      return nullptr;
    }
    if (autograd_meta->grad_fn_) {
      throw std::logic_error(
          "grad_accumulator() should be only called on leaf Variables");
    }
    if (!autograd_meta->requires_grad_) {
      return nullptr;
    }

    std::lock_guard<std::mutex> lock(autograd_meta->mutex_);

    auto result = autograd_meta->grad_accumulator_.lock();
    if (result)
      return result;

    c10::raw::intrusive_ptr::incref(self.unsafeGetTensorImpl());
    auto intrusive_from_this = c10::intrusive_ptr<at::TensorImpl>::reclaim(self.unsafeGetTensorImpl());
    result = std::make_shared<AccumulateGrad>(Variable(std::move(intrusive_from_this)));
    autograd_meta->grad_accumulator_ = result;
    return result;
  }

  Edge gradient_edge(const Variable& self) {
    // If grad_fn is null (as is the case for a leaf node), we instead
    // interpret the gradient function to be a gradient accumulator, which will
    // accumulate its inputs into the grad property of the variable. These
    // nodes get suppressed in some situations, see "suppress gradient
    // accumulation" below. Note that only variables which have `requires_grad =
    // True` can have gradient accumulators.
    if (const auto& gradient = self.grad_fn()) {
      return Edge(gradient, self.output_nr());
    } else {
      return Edge(grad_accumulator(self), 0);
    }
  }

  void set_gradient_edge(const Variable& self, Edge edge) {
    auto* meta = materialize_autograd_meta(self);
    meta->grad_fn_ = std::move(edge.function);
    meta->output_nr_ = edge.input_nr;
  }

  Node* grad_fn_unsafe(const Variable& self) {
    if (get_autograd_meta(self)) {
      return get_autograd_meta(self)->grad_fn_.get();
    } else {
      return nullptr;
    }
  }

  // Versions
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  void set_version_counter(
      const Variable& self,
      const c10::VariableVersion& version_counter) {
    TORCH_CHECK(self.defined(), "cannot call set_version_counter() on undefined tensor");
    self.unsafeGetTensorImpl()->set_version_counter(version_counter);
  }

  void bump_version(const Variable& self) {
    TORCH_CHECK(self.defined(), "cannot call bump_version() on undefined tensor");
    self.unsafeGetTensorImpl()->bump_version();
  }

  const c10::VariableVersion& version_counter(const Variable& self) {
    TORCH_CHECK(self.defined(), "cannot call version_counter() on undefined tensor");
    return self.unsafeGetTensorImpl()->version_counter();
  }

  // Hooks
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  void add_hook(const Variable& self, std::shared_ptr<FunctionPreHook> hook) {
    materialize_autograd_meta(self)->hooks_.push_back(std::move(hook));
  }

  namespace {
    std::vector<std::shared_ptr<FunctionPreHook>> empty_singleton;
  }

  // TODO: Return an ArrayRef instead (and delete the singleton while you're at
  // it
  const std::vector<std::shared_ptr<FunctionPreHook>>& hooks(const Variable& self)
      {
    if (get_autograd_meta(self)) {
      return get_autograd_meta(self)->hooks_;
    } else {
      return empty_singleton;
    }
  }

  void clear_hooks(const Variable& self) {
    // This is a little goofy, but usually this should be a no oop
    materialize_autograd_meta(self)->hooks_.clear();
  }

  void set_name(const Variable& self, const std::string& name) {
    materialize_autograd_meta(self)->name_ = name;
  }

  // Miscellaneous
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  void set_pyobj(const Variable& self, PyObject* pyobj) {
    TORCH_CHECK(self.defined(), "cannot call set_pyobj() on undefined tensor");
    self.unsafeGetTensorImpl()->set_pyobj(pyobj);
  }

  PyObject* pyobj(const Variable& self) {
    TORCH_CHECK(self.defined(), "cannot call pyobj() on undefined tensor");
    return self.unsafeGetTensorImpl()->pyobj();
  }

  AutogradMeta* get_autograd_meta(const Variable& self) {
    // NB: could return null
    TORCH_CHECK(self.defined(), "cannot call get_autograd_meta() on undefined tensor");
    return static_cast<AutogradMeta*>(self.unsafeGetTensorImpl()->autograd_meta());
  }

} // namespace impl

using at::Tensor;

struct VariableHooks final : at::impl::VariableHooksInterface {
  Tensor tensor_data(const Tensor&) const override;
  Tensor variable_data(const Tensor&) const override;
  const std::shared_ptr<torch::autograd::Node>& grad_fn(const Tensor&) const override;
  unsigned _register_hook(const Tensor&, std::function<Tensor(const Tensor&)> hook) const override;
  void remove_hook(const Tensor&, unsigned pos) const override;
  bool is_view(const Tensor&) const override;
  const Tensor& base(const Tensor&) const override;
  const std::string& name(const Tensor&) const override;
};

VariableHooks variableHooks;
at::impl::VariableHooksRegisterer registerVariableHooks(&variableHooks);

Tensor VariableHooks::variable_data(const Tensor& self) const {
  TORCH_CHECK(self.defined(), "cannot call variable_data() on undefined tensor");
  auto self_impl_copy = self.unsafeGetTensorImpl()->shallow_copy_and_detach(
    /*version_counter=*/0,
    /*allow_tensor_metadata_change=*/false);
  self_impl_copy->set_autograd_meta(nullptr);
  return at::Tensor(self_impl_copy);
}

Tensor VariableHooks::tensor_data(const Tensor& self) const {
  TORCH_CHECK(self.defined(), "cannot call tensor_data() on undefined tensor");
  auto self_impl_copy = self.unsafeGetTensorImpl()->shallow_copy_and_detach(
    /*version_counter=*/self.unsafeGetTensorImpl()->version_counter(),
    /*allow_tensor_metadata_change=*/self.unsafeGetTensorImpl()->allow_tensor_metadata_change());
  return at::Tensor(self_impl_copy);
}

// View Variables
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

bool VariableHooks::is_view(const Tensor& self) const {
  if (torch::autograd::impl::get_autograd_meta(self)) {
    return torch::autograd::impl::get_autograd_meta(self)->is_view_;
  } else {
    return false;
  }
}

const Tensor& VariableHooks::base(const Tensor& self) const {
  if (self.is_view()) {
    // is_view() implies get_autograd_meta()
    auto diff_view_meta = static_cast<torch::autograd::DifferentiableViewMeta*>(torch::autograd::impl::get_autograd_meta(self));
    return diff_view_meta->base_;
  } else {
    throw std::runtime_error("Can't get base of non-view Variable");
  }
}

namespace {
  std::string singleton_string;
}

const std::string& VariableHooks::name(const Tensor& self) const {
  TORCH_CHECK(self.defined(), "cannot call variable_data() on undefined tensor");
  if (torch::autograd::impl::get_autograd_meta(self)) {
    return torch::autograd::impl::get_autograd_meta(self)->name_;
  } else {
    return singleton_string;
  }
}

namespace {
  std::shared_ptr<torch::autograd::Node> singleton_shared_ptr;
}

const std::shared_ptr<torch::autograd::Node>& VariableHooks::grad_fn(const Tensor& self) const {
  if (self.is_view()) {
    // NB: is_view() ==> get_autograd_meta()
    auto diff_view_meta = static_cast<torch::autograd::DifferentiableViewMeta*>(torch::autograd::impl::get_autograd_meta(self));
    std::lock_guard<std::mutex> lock(diff_view_meta->mutex_);
    if (!diff_view_meta->grad_fn_ && !diff_view_meta->base_.requires_grad()) {
      return diff_view_meta->grad_fn_;
    }
    auto current_version = self._version();
    if (diff_view_meta->attr_version != current_version) {
      AT_ASSERT(diff_view_meta->output_nr_ == 0);
      auto fn = std::make_shared<torch::autograd::generated::AsStridedBackward>();
      fn->self_geometry = at::TensorGeometry(diff_view_meta->base_);
      fn->size = self.sizes().vec();
      fn->stride = self.strides().vec();
      fn->storage_offset = self.storage_offset();
      fn->set_next_edges(torch::autograd::collect_next_edges(diff_view_meta->base_));
      fn->add_input_metadata(
        diff_view_meta->base_.type()
      , self.sizes() // Note: sizes(), not base_.sizes(), is intentional
      , diff_view_meta->base_.device());
      diff_view_meta->grad_fn_ = std::move(fn);
      diff_view_meta->attr_version = current_version;
    }
    return diff_view_meta->grad_fn_;
  } else {
    if (torch::autograd::impl::get_autograd_meta(self)) {
      return torch::autograd::impl::get_autograd_meta(self)->grad_fn_;
    } else {
      return singleton_shared_ptr;
    }
  }
}

void VariableHooks::remove_hook(const Tensor& self, unsigned pos) const {
  auto &map = torch::autograd::impl::materialize_autograd_meta(self)->cpp_hooks_map;
  TORCH_CHECK(map && pos < list->size() , "Invalid index, no hook at position ", pos);
  // Hook will be ignored
  (*map)[pos] = nullptr;
}

unsigned VariableHooks::_register_hook(const Tensor& self, std::function<Tensor(const Tensor&)> hook) const {
  TORCH_CHECK(self.requires_grad(), "cannot register a hook on a variable that "
                           "doesn't require gradient");
  // NB: materialize_autograd_meta unnecessary due to requires grad check
  auto &map = torch::autograd::impl::get_autograd_meta(self)->cpp_hooks_map;
  if(!map) {
    torch::autograd::impl::create_cpp_hook(self);
  }
  unsigned idx = map->size();
  map->insert(idx, hook);
  return idx;
}

}} // namespace torch::autograd
