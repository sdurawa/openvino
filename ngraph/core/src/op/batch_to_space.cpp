// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <cmath>
#include <cstddef>
#include <memory>
#include <ngraph/ops.hpp>
#include <ngraph/validation_util.hpp>
#include <numeric>
#include "itt.hpp"

#include "ngraph/builder/make_constant.hpp"
#include "ngraph/node.hpp"
#include "ngraph/op/batch_to_space.hpp"
#include "ngraph/opsets/opset3.hpp"
#include "ngraph/shape.hpp"

#include "ngraph/runtime/opt_kernel/reshape.hpp"
#include "ngraph/runtime/reference/strided_slice.hpp"
#include "ngraph/slice_plan.hpp"

using namespace std;
using namespace ngraph;

NGRAPH_RTTI_DEFINITION(op::v1::BatchToSpace, "BatchToSpace", 1);

ngraph::op::v1::BatchToSpace::BatchToSpace(const ngraph::Output<ngraph::Node>& data,
                                           const ngraph::Output<ngraph::Node>& block_shape,
                                           const ngraph::Output<ngraph::Node>& crops_begin,
                                           const ngraph::Output<ngraph::Node>& crops_end)
    : Op({data, block_shape, crops_begin, crops_end})
{
    constructor_validate_and_infer_types();
}

void op::v1::BatchToSpace::validate_and_infer_types()
{
    NGRAPH_OP_SCOPE(v1_BatchToSpace_validate_and_infer_types);

    const auto& data_et = get_input_element_type(0);
    const auto& block_shape_et = get_input_element_type(1);
    const auto& crops_begin_et = get_input_element_type(2);
    const auto& crops_end_et = get_input_element_type(3);

    element::Type inputs_integer_et{};
    NODE_VALIDATION_CHECK(
        this,
        element::Type::merge(inputs_integer_et, crops_begin_et, crops_end_et) &&
            element::Type::merge(inputs_integer_et, inputs_integer_et, block_shape_et),
        "block_shape, crops_begin and crops_end inputs must have same element type. Got: ",
        block_shape_et,
        ", ",
        crops_begin_et,
        " and ",
        crops_end_et);

    NODE_VALIDATION_CHECK(this,
                          inputs_integer_et.is_integral_number(),
                          "block_shape and crops inputs must have integer element type. Got: ",
                          inputs_integer_et);

    const PartialShape& data_pshape = get_input_partial_shape(0);
    const PartialShape& block_shape_ps = get_input_partial_shape(1);
    const PartialShape& crops_begin_ps = get_input_partial_shape(2);
    const PartialShape& crops_end_ps = get_input_partial_shape(3);

    PartialShape inputs_same_ps{PartialShape::dynamic()};
    NODE_VALIDATION_CHECK(
        this,
        PartialShape::merge_into(inputs_same_ps, crops_begin_ps) &&
            PartialShape::merge_into(inputs_same_ps, crops_end_ps) &&
            PartialShape::merge_into(inputs_same_ps, block_shape_ps),
        "block_shape, crops_begin and crops_end inputs must have the same shape. Got: ",
        block_shape_ps,
        ", ",
        crops_begin_ps,
        " and ",
        crops_end_ps);

    const Rank inputs_rank_one = inputs_same_ps.rank();
    NODE_VALIDATION_CHECK(this,
                          inputs_rank_one.compatible(1),
                          "block_shape and crops inputs must have rank 1. Got: ",
                          inputs_rank_one);

    const Rank data_rank = data_pshape.rank();
    if (data_rank.is_static())
    {
        NODE_VALIDATION_CHECK(this,
                              (data_rank.get_length() >= 2),
                              "data input must have rank greater or equal than 2. Got: ",
                              data_rank.get_length());

        if (inputs_same_ps.is_static())
        {
            NODE_VALIDATION_CHECK(this,
                                  data_rank.get_length() == inputs_same_ps[0].get_length(),
                                  "block_shape and crop inputs must have same number of elements "
                                  "as data input rank. Got: ",
                                  inputs_same_ps[0],
                                  " and ",
                                  data_rank);
        }
    }

    const auto block_const = get_constant_from_source(input_value(1));
    const auto crops_begin_const = get_constant_from_source(input_value(2));
    const auto crops_end_const = get_constant_from_source(input_value(3));

    if (block_const && crops_begin_const && crops_end_const && data_pshape.is_static())
    {
        const Shape& data_sshape = data_pshape.to_shape();

        auto block_val = block_const->cast_vector<int64_t>();
        auto crops_begin_val = crops_begin_const->cast_vector<int64_t>();
        auto crops_end_val = crops_end_const->cast_vector<int64_t>();

        bool block_vals_valid =
            std::all_of(begin(block_val), end(block_val), [](int64_t elem) { return elem >= 1; });
        NODE_VALIDATION_CHECK(this,
                              block_vals_valid,
                              "Elements of block_shape input must be greater or equal to one.");

        bool crops_begin_vals_valid = std::all_of(
            begin(crops_begin_val), end(crops_begin_val), [](int64_t elem) { return elem >= 0; });
        bool crops_end_vals_valid = std::all_of(
            begin(crops_end_val), end(crops_end_val), [](int64_t elem) { return elem >= 0; });
        NODE_VALIDATION_CHECK(
            this,
            crops_begin_vals_valid && crops_end_vals_valid,
            "Elements of crops_begin and crops_end inputs must be greater or equal to zero.");

        int64_t block_prod =
            std::accumulate(begin(block_val), end(block_val), 1, std::multiplies<int64_t>());

        NODE_VALIDATION_CHECK(this,
                              data_sshape[0] % block_prod == 0,
                              "The input data's 'batch' axis size: ",
                              data_sshape[0],
                              " must be a multiple of",
                              " product of block_shape values: ",
                              block_prod);

        for (size_t idx = 0; idx < data_sshape.size(); idx++)
        {
            const bool is_valid_crops_and_shape =
                crops_begin_val[idx] + crops_end_val[idx] <=
                block_val[idx] * static_cast<int64_t>(data_sshape[idx]);
            NODE_VALIDATION_CHECK(this,
                                  is_valid_crops_and_shape,
                                  "crops_begin[i] + crops_end[i] must be less or equal to "
                                  "block_shape[i] * input_shape[i]");
        }

        Shape output_sshape = {static_cast<size_t>(data_sshape[0] / block_prod)};
        for (size_t idx = 1; idx < data_sshape.size(); ++idx)
        {
            output_sshape.push_back(static_cast<size_t>(data_sshape[idx] * block_val[idx] -
                                                        crops_begin_val[idx] - crops_end_val[idx]));
        }

        set_output_size(1);
        set_output_type(0, data_et, output_sshape);
    }
    else
    {
        set_output_type(0, data_et, PartialShape::dynamic(data_rank));
    }
}

std::shared_ptr<ngraph::Node>
    ngraph::op::v1::BatchToSpace::clone_with_new_inputs(const OutputVector& new_args) const
{
    NGRAPH_OP_SCOPE(v1_BatchToSpace_clone_with_new_inputs);
    check_new_args_count(this, new_args);
    return make_shared<BatchToSpace>(
        new_args.at(0), new_args.at(1), new_args.at(2), new_args.at(3));
}

bool ngraph::op::v1::BatchToSpace::visit_attributes(ngraph::AttributeVisitor& visitor)
{
    NGRAPH_OP_SCOPE(v1_BatchToSpace_visit_attributes);
    return true;
}

namespace
{
    bool batch_to_space_evaluate(const HostTensorVector& outputs, const HostTensorVector& inputs)
    {
        auto data = inputs[0];
        size_t elem_size = data->get_element_type().size();

        if (data->get_partial_shape().is_dynamic())
        {
            return false;
        }
        auto data_shape = data->get_shape();
        auto data_rank = data_shape.size();
        if (data_rank < 2)
        {
            return false;
        }

        size_t block_values_size = shape_size(inputs[1]->get_shape());
        size_t crops_begin_size = shape_size(inputs[2]->get_shape());
        size_t crops_end_size = shape_size(inputs[3]->get_shape());
        NGRAPH_CHECK(
            block_values_size == data_rank && crops_begin_size == data_rank &&
                crops_end_size == data_rank,
            "Invalid block_shape/crops_begin/crops_end shape with respect to rank of data input");

        const auto* block_values = inputs[1]->get_data_ptr<int64_t>();
        const auto* crops_begin_values = inputs[2]->get_data_ptr<int64_t>();
        const auto* crops_end_values = inputs[3]->get_data_ptr<int64_t>();

        const bool block_vals_valid = std::all_of(
            block_values, block_values + block_values_size, [](int64_t elem) { return elem >= 1; });
        NGRAPH_CHECK(block_vals_valid, "Invalid element values of block_shape input");

        const bool crops_begin_vals_valid = std::all_of(crops_begin_values,
                                                        crops_begin_values + crops_begin_size,
                                                        [](int64_t elem) { return elem >= 0; });
        const bool crops_end_vals_valid = std::all_of(crops_end_values,
                                                      crops_end_values + crops_end_size,
                                                      [](int64_t elem) { return elem >= 0; });
        NGRAPH_CHECK(crops_begin_vals_valid && crops_end_vals_valid,
                     "Invalid element values of crops_begin/crops_end input/s");

        const std::size_t block_prod = std::accumulate(
            block_values, block_values + block_values_size, 1UL, std::multiplies<std::size_t>());
        NGRAPH_CHECK(data_shape[0] % block_prod == 0,
                     "Invalid batch axis of data input with respect to block_shape values");

        for (size_t i = 0; i < data_rank; i++)
        {
            const bool is_valid_crops_and_shape =
                crops_begin_values[i] + crops_end_values[i] <=
                block_values[i] * static_cast<int64_t>(data_shape[i]);
            NGRAPH_CHECK(
                is_valid_crops_and_shape,
                "Invalid crops values (out of bounds) with respect to the shape of data input");
        }

        Shape dispersed_shape(1);
        dispersed_shape.insert(dispersed_shape.end(), data_shape.begin(), data_shape.end());
        std::vector<size_t> axes_order(block_values_size + 1);
        std::vector<size_t> plain_axes_order(block_values_size + 1);
        std::iota(plain_axes_order.begin(), plain_axes_order.end(), 0);
        Shape squeezed_shape(data_shape.begin(), data_shape.end());
        if (squeezed_shape.size() > block_values_size)
        {
            return false;
        }

        auto* flat_data = data->get_data_ptr<char>();
        std::vector<char> dispersed_data(shape_size(data_shape) * elem_size);

        Shape post_transpose_shape(axes_order.size());
        std::vector<char> post_transpose_data(shape_size(data_shape) * elem_size);

        for (size_t block_idx = 1; block_idx < block_values_size; ++block_idx)
        {
            dispersed_shape[0] = block_values[block_idx];
            dispersed_shape[1] /= block_values[block_idx];
            runtime::opt_kernel::reshape(flat_data,
                                         dispersed_data.data(),
                                         data_shape,
                                         plain_axes_order,
                                         dispersed_shape,
                                         elem_size);

            size_t val = 1;
            for (size_t axis_idx = 0; axis_idx <= block_values_size; ++axis_idx)
            {
                if ((block_idx + 1) == axis_idx)
                {
                    axes_order[axis_idx] = 0;
                }
                else
                {
                    axes_order[axis_idx] = val;
                    val++;
                }
            }
            for (size_t axis_idx = 0; axis_idx < axes_order.size(); ++axis_idx)
            {
                post_transpose_shape[axis_idx] = dispersed_shape[axes_order[axis_idx]];
            }

            runtime::opt_kernel::reshape(dispersed_data.data(),
                                         post_transpose_data.data(),
                                         dispersed_shape,
                                         axes_order,
                                         post_transpose_shape,
                                         elem_size);
            squeezed_shape[0] = dispersed_shape[1];
            squeezed_shape[block_idx] *= block_values[block_idx];
            dispersed_shape[block_idx + 1] = squeezed_shape[block_idx];
            runtime::opt_kernel::reshape(post_transpose_data.data(),
                                         flat_data,
                                         post_transpose_shape,
                                         plain_axes_order,
                                         squeezed_shape,
                                         elem_size);
            data_shape = squeezed_shape;
        }

        std::vector<int64_t> upperbounds_values(data_shape.size());
        for (size_t i = 0; i < data_shape.size(); ++i)
        {
            upperbounds_values[i] = data_shape[i] - crops_end_values[i];
        }

        std::vector<size_t> begin_mask(data_shape.size(), 0);
        std::vector<size_t> end_mask(data_shape.size(), 0);

        std::vector<int64_t> begins(shape_size(inputs[2]->get_shape()));
        begins.assign(crops_begin_values, crops_begin_values + shape_size(inputs[2]->get_shape()));

        std::vector<int64_t> default_strides(begins.size(), 1);
        SlicePlan slice_plan = make_slice_plan(data_shape,
                                               begins,
                                               upperbounds_values,
                                               default_strides,
                                               begin_mask,
                                               end_mask,
                                               AxisSet(),
                                               AxisSet(),
                                               AxisSet());
        runtime::reference::strided_slice(
            flat_data, outputs[0]->get_data_ptr<char>(), data_shape, slice_plan, elem_size);
        return true;
    }
} // namespace

bool ngraph::op::v1::BatchToSpace::evaluate(const HostTensorVector& outputs,
                                            const HostTensorVector& inputs) const
{
    NGRAPH_OP_SCOPE(v1_BatchToSpace_evaluate);
    NGRAPH_CHECK(validate_host_tensor_vector(inputs, 4));
    NGRAPH_CHECK(validate_host_tensor_vector(outputs, 1));
    return batch_to_space_evaluate(outputs, inputs);
}

bool ngraph::op::v1::BatchToSpace::has_evaluate() const
{
    NGRAPH_OP_SCOPE(v1_BatchToSpace_has_evaluate);
    return !get_input_partial_shape(0).is_dynamic() && get_input_shape(0).size() >= 2 &&
           get_input_shape(0).size() <= shape_size(get_input_shape(1));
}
