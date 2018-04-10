#include "xchainer/numerical_gradient.h"

#include <algorithm>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>
#include <gsl/gsl>
#include <nonstd/optional.hpp>

#include "xchainer/array.h"
#include "xchainer/check_backward.h"
#include "xchainer/context.h"
#include "xchainer/indexable_array.h"
#include "xchainer/indexer.h"
#include "xchainer/native/native_backend.h"
#include "xchainer/op_node.h"
#include "xchainer/shape.h"
#include "xchainer/testing/array.h"
#include "xchainer/testing/device_session.h"

namespace xchainer {
namespace {

using Arrays = std::vector<Array>;
using Fprop = std::function<std::vector<Array>(const std::vector<Array>&)>;

Arrays ForwardWithIncorrectBackward(const Arrays& inputs) {
    const Array& in = inputs[0];
    Array out = EmptyLike(in);

    auto backward_function = [](const Array& gout, const std::vector<GraphId>&) { return gout * gout; };
    internal::SetUpOpNodes("incorrect_unary", {in}, out, {backward_function});

    VisitDtype(in.dtype(), [&](auto pt) {
        using T = typename decltype(pt)::type;
        IndexableArray<const T> in_iarray{in};
        IndexableArray<T> out_iarray{out};
        Indexer indexer{out.shape()};

        for (int64_t i = 0; i < indexer.total_size(); ++i) {
            indexer.Set(i);
            out_iarray[indexer] = in_iarray[indexer];
        }
    });

    return {out};
}

class CheckBackwardTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        device_session_.emplace(DeviceId{native::NativeBackend::kDefaultName, 0});
        requires_grad_ = GetParam();
    }

    void TearDown() override { device_session_.reset(); }

protected:
    template <typename Data>
    void CheckCheckBackward(
            bool expect_correct,
            const Fprop& fprop,
            const Shape& shape,
            Data input_data,
            Data grad_output_data,
            Data eps_data,
            double atol,
            double rtol,
            const GraphId& graph_id) {
        Arrays inputs{testing::BuildArray(shape, input_data)};
        if (requires_grad_) {
            inputs[0].RequireGrad(graph_id);
        }

        Arrays grad_outputs{testing::BuildArray(shape, grad_output_data)};
        Arrays eps{testing::BuildArray(shape, eps_data)};

        bool is_none_of_grad_required =
                std::none_of(inputs.begin(), inputs.end(), [graph_id](const Array& input) { return input.IsGradRequired(graph_id); });
        if (expect_correct || is_none_of_grad_required) {
            // We cannot expect any failures in case none of the input std::vector<Array> require gradients
            EXPECT_NO_THROW(CheckBackwardComputation(fprop, inputs, grad_outputs, eps, atol, rtol, graph_id));
        } else {
            // Catch the gtest failure expected to be generated by CheckBackwardComputation but without failing this test
            EXPECT_THROW(CheckBackwardComputation(fprop, inputs, grad_outputs, eps, atol, rtol, graph_id), GradientCheckError);
        }
    }

private:
    nonstd::optional<testing::DeviceSession> device_session_;
    bool requires_grad_{};
};

class CheckDoubleBackwardTest : public ::testing::Test {
protected:
    void SetUp() override { device_session_.emplace(DeviceId{native::NativeBackend::kDefaultName, 0}); }

    void TearDown() override { device_session_.reset(); }

protected:
    template <typename Data>
    void CheckCheckDoubleBackward(
            const Fprop& fprop,
            const Shape& shape,
            Data input_data,
            Data grad_output_data,
            Data grad_grad_input_data,
            Data eps_input_data,
            Data eps_grad_output_data,
            double atol,
            double rtol,
            const GraphId& graph_id) {
        Arrays inputs{testing::BuildArray(shape, input_data)};
        Arrays grad_outputs{testing::BuildArray(shape, grad_output_data)};
        Arrays grad_grad_inputs{testing::BuildArray(shape, grad_grad_input_data)};
        Arrays eps{testing::BuildArray(shape, eps_input_data), testing::BuildArray(shape, eps_grad_output_data)};

        for (auto& input : inputs) {
            input.RequireGrad(graph_id);
        }
        for (auto& grad_output : grad_outputs) {
            grad_output.RequireGrad(graph_id);
        }

        // A failure occurs if backward computation and numerical gradients have differences
        CheckDoubleBackwardComputation(fprop, inputs, grad_outputs, grad_grad_inputs, eps, atol, rtol, graph_id);
    }

private:
    nonstd::optional<testing::DeviceSession> device_session_;
};

TEST_P(CheckBackwardTest, CorrectBackward) {
    using Data = std::array<float, 3>;
    Data input_data{1.f, 2.f, 1.f};
    Data grad_output_data{0.f, -2.f, 1.f};
    Data eps_data{1e-3f, 1e-3f, 1e-3f};
    Fprop fprop = [](const Arrays& inputs) -> Arrays { return {inputs[0] * inputs[0]}; };
    CheckCheckBackward(true, fprop, {1, 3}, input_data, grad_output_data, eps_data, 1e-5, 1e-4, "graph_1");
}

TEST_P(CheckBackwardTest, IncorrectBackward) {
    using Data = std::array<float, 3>;
    Data input_data{-2.f, 3.f, 1.f};
    Data grad_output_data{0.f, -2.f, 1.f};
    Data eps_data{1e-3f, 1e-3f, 1e-3f};
    CheckCheckBackward(false, &ForwardWithIncorrectBackward, {1, 3}, input_data, grad_output_data, eps_data, 1e-5, 1e-4, "graph_1");
}

TEST_F(CheckDoubleBackwardTest, CorrectBackward) {
    using Data = std::array<float, 3>;
    Data input_data{1.f, 2.f, 3.f};
    Data grad_output_data{1.f, 1.f, 1.f};
    Data grad_grad_input_data{1.f, 1.f, 1.f};
    Data eps_input_data{1e-3f, 1e-3f, 1e-3f};
    Data eps_grad_output_data{1e-3f, 1e-3f, 1e-3f};
    Fprop fprop = [](const Arrays& inputs) -> Arrays { return {inputs[0] * inputs[0]}; };
    CheckCheckDoubleBackward(
            fprop, {1, 3}, input_data, grad_output_data, grad_grad_input_data, eps_input_data, eps_grad_output_data, 1e-4, 1e-3, "graph_1");
}

INSTANTIATE_TEST_CASE_P(ForEachSingleSetRequiresGrad, CheckBackwardTest, ::testing::Bool());

}  // namespace
}  // namespace xchainer
