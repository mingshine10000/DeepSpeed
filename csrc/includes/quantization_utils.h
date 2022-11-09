#include <cstdio>
#include "conversion_utils.h"
#include "ds_kernel_utils.h"
#include "memory_access_utils.h"
#include "quantization.h"
#include "reduction_utils.h"

#pragma once

using rop = reduce::ROpType;

namespace quantize {
constexpr int granularity = 16;
constexpr int h_per_load = granularity / sizeof(__half);
constexpr int h2_per_load = granularity / sizeof(__half2);
constexpr int max_threads = 1024;

/*
Group stats tracks the necessary statistics about the quantized group
to abstract the particulars for the main loop. The scales in particular
can be derived from
*/
template <Type qType>
class GroupStats {
public:
    DS_D_INLINE void update(__half2 val);

    DS_D_INLINE void reduce(cg::thread_block& tb, cg::thread_block_tile<hw_warp_size>& warp);
};

template <>
class GroupStats<Type::Symmetric> {
public:
    // Symmetric quantization only tracks the maximum absolute value
    __half2 cur_max;
    float max;

    /*
    Technically, this would give bad results if there
    are 0 values to process since the reduction would
    give -inf instead of 0. We do not consider this
    to be a reasonable edge case.
    */
    DS_D_INLINE GroupStats() { cur_max = reduce::init<rop::Max, __half2>(); }

    /*
    Updated the running absmax used to calculate params.
    Function Arguments :
        val : The __half2 value to update the running min and max with.
    */
    DS_D_INLINE void update(__half2 val)
    {
        cur_max = reduce::element<rop::Max>(cur_max, __habs2(val));
    }

    /*
    Functiuon to Return calculated Quantization params.
    Template Arguments :
        numBits -   Number of bits in quantized element.    int : 8 or 4
    Function Arguments :
        tb      -   Threadblock object. cg::thread_block
        warp    -   Warp object.        cg::thread_block_tile<hw_warp_size>
    */
    DS_D_INLINE void reduce(cg::thread_block& tb, cg::thread_block_tile<hw_warp_size>& warp)
    {
        const float2 partial_max = conversion::to<float2>(cur_max);
        max = reduce::element<rop::Max>(partial_max.x, partial_max.y);

        reduce::block<rop::Max>(tb, warp, max);
    }
};

template <>
class GroupStats<Type::IntegerSymmetric> {
public:
    // Symmetric quantization only tracks the maximum absolute value
    __half2 cur_max;
    float max;

    /*
    Technically, this would give bad results if there
    are 0 values to process since the reduction would
    give -inf instead of 0. We do not consider this
    to be a reasonable edge case.
    */
    DS_D_INLINE GroupStats() { cur_max = reduce::init<rop::Max, __half2>(); }

    /*
    Updated the running absmax used to calculate params.
    Function Arguments :
        val : The __half2 value to update the running min and max with.
    */
    DS_D_INLINE void update(__half2 val)
    {
        cur_max = reduce::element<rop::Max>(cur_max, __habs2(val));
    }

    /*
    Functiuon to Return calculated Quantization params.
    Template Arguments :
        numBits -   Number of bits in quantized element.    int : 8 or 4
    Function Arguments :
        tb      -   Threadblock object. cg::thread_block
        warp    -   Warp object.        cg::thread_block_tile<hw_warp_size>
    */
    DS_D_INLINE void reduce(cg::thread_block& tb, cg::thread_block_tile<hw_warp_size>& warp)
    {
        const float2 partial_max = conversion::to<float2>(cur_max);
        max = reduce::element<rop::Max>(partial_max.x, partial_max.y);

        reduce::block<rop::Max>(tb, warp, max);
    }
};

template <>
class GroupStats<Type::Asymmetric> {
public:
    __half2 cur_max;
    __half2 cur_min;
    float max;
    float min;

    /*
    Initialize cur_max to -inf, cur_min to inf since
    we are doing a true range analysis.
    */
    DS_D_INLINE GroupStats()
    {
        cur_max = reduce::init<rop::Max, __half2>();
        cur_min = reduce::init<rop::Min, __half2>();
    }

    /*
    Updated the running min and max used to calculate params.
    Function Arguments :
        val : The __half2 value to update the running min and max with.
    */
    DS_D_INLINE void update(__half2 val)
    {
        cur_max = reduce::element<rop::Max>(cur_max, val);
        cur_min = reduce::element<rop::Min>(cur_min, val);
    }

    /*
    Functiuon to Return calculated Quantization params.
    Template Arguments :
        numBits -   Number of bits in quantized element.    int : 8 or 4
    Function Arguments :
        tb      -   Threadblock object. cg::thread_block
        warp    -   Warp object.        cg::thread_block_tile<hw_warp_size>
    */
    DS_D_INLINE void reduce(cg::thread_block& tb, cg::thread_block_tile<hw_warp_size>& warp)
    {
        const float2 partial_max = conversion::to<float2>(cur_max);
        max = reduce::element<rop::Max>(partial_max.x, partial_max.y);

        const float2 partial_min = conversion::to<float2>(cur_min);
        min = reduce::element<rop::Min>(partial_min.x, partial_min.y);

        reduce::block<rop::Max, rop::Min>(tb, warp, max, min);
    }
};

/*
Class to hold the quantization parameters for a given tensor.
Holds the implementation of the quantization operation.
*/
template <Type qType, int numBits>
class Params {
public:
    /*
    QUantization implementation, Supports
    1) 4 Bit
    2) 8 Bit
    3) Symmetric
    4) Asymmetric
    Function Arguments :
        val : The __half value to Quantize.
    */
    DS_D_INLINE int8_t quantize(__half val);

    DS_D_INLINE void store(float* params, int group_index);
};

template <int numBits>
class Params<Type::Symmetric, numBits> {
public:
    float scale;

    DS_D_INLINE Params(GroupStats<Type::Symmetric> stats)
    {
        if (stats.max == 0) {
            scale = 1.0;
        } else {
            scale = (1 << numBits) / (2 * stats.max);
        }
    }

    DS_D_INLINE int8_t quantize(__half val)
    {
        constexpr int32_t q_min = -(1 << (numBits - 1));
        constexpr int32_t q_max = (1 << (numBits - 1)) - 1;

        float val_f = conversion::to<float>(val) * scale;
        int32_t data_i32 = conversion::to<int32_t>(val_f);
        data_i32 = min(max(data_i32, q_min), q_max);
        return (int8_t)data_i32;
    }

    DS_D_INLINE void store(float* params, int group_index)
    {
        const float store_scale = 1 / scale;
        mem_access::store_global<sizeof(float)>(params + group_index, &store_scale);
    }
};

template <int numBits>
class Params<Type::IntegerSymmetric, numBits> {
public:
    int32_t scale;

    DS_D_INLINE Params(GroupStats<Type::IntegerSymmetric> stats)
    {
        scale = conversion::to<int32_t>(stats.max + 0.5f);
    }

    DS_D_INLINE int8_t quantize(__half val)
    {
        constexpr int32_t q_max = (1 << (numBits - 1)) - 1;
        float val_f = conversion::to<float>(val) * q_max;
        float scaled_val = val_f / conversion::to<float>(scale);
        int32_t data_i32 = conversion::to<int32_t>(scaled_val);
        return (int8_t)data_i32;
    }

    DS_D_INLINE void store(float* params, int group_index)
    {
        mem_access::store_global<sizeof(float)>(params + group_index, &scale);
    }
};

template <int numBits>
class Params<Type::Asymmetric, numBits> {
public:
    float scale;
    float offset;

    DS_D_INLINE Params(GroupStats<Type::Asymmetric> stats)
    {
        if (stats.max == stats.min) {
            scale = 1.0;
        } else {
            scale = (1 << numBits) / (stats.max - stats.min);
        }
        offset = -(1 << (numBits - 1)) - (stats.min * scale);
    }

    DS_D_INLINE int8_t quantize(__half val)
    {
        constexpr int32_t q_min = -(1 << (numBits - 1));
        constexpr int32_t q_max = (1 << (numBits - 1)) - 1;

        float val_f = conversion::to<float>(val) * scale + offset;
        int32_t data_i32 = conversion::to<int32_t>(val_f);
        data_i32 = min(max(data_i32, q_min), q_max);
        return (int8_t)data_i32;
    }

    DS_D_INLINE void store(float* params, int group_index)
    {
        const float store_scale = 1 / scale;
        mem_access::store_global<sizeof(float)>(params + 2 * group_index, &store_scale);
        mem_access::store_global<sizeof(float)>(params + 2 * group_index + 1, &offset);
    }
};

/*
Helper function to do parallel reduction and calculate params.
Template Arguments :
    qType           -   Type of quantization to perform.                            Type::Symmetric
or Type::Asymmetric numChunks       -   Number of bits in quantized element. int : 8 or 4
    elemsPerBlock   -   Number of elements to participate in Qunatization together. int : 8 or 4
Function Arguments :
    tb      -   Threadblock object. cg::thread_block
    warp    -   Warp object.        cg::thread_block_tile<hw_warp_size>
    stats   -   Group stats         GroupStats<qType>
*/
template <Type qType, int numBits, int elemsPerBlock>
DS_D_INLINE Params<qType, numBits> _get_params(cg::thread_block& tb,
                                               cg::thread_block_tile<hw_warp_size>& warp,
                                               GroupStats<qType> stats);

template <Type qType, int numBits, int elemsPerBlock>
DS_D_INLINE Params<qType, numBits> _get_params(cg::thread_block& tb,
                                               cg::thread_block_tile<hw_warp_size>& warp,
                                               GroupStats<qType> stats)
{
    stats.reduce(tb, warp);

    Params<qType, numBits> params(stats);

    return params;
}

/*
The kernel that quantizes 16 bytes of __half type input data.
Template Arguments :
    numBits -   Number of bits in quantized element.    int : 8 or 4
    qType   - Type of quantization to perform.          Type::Symmetric or Type::Asymmetric
Function Arguments :
    local_output -  Pointer to shared memory to store quantized data.   int8_t*
    data         -  Pointer to input data.                              __half*
    Params       -  Parameters for quantization.                        Params<qType, numBits>
*/
template <int numBits, Type qType>
DS_D_INLINE void _chunk(int8_t* local_output, const __half* data, Params<qType, numBits> q_params);

/*
The kernel that quantizes 16 bytes of __half2 type input data.
Template Arguments :
    numBits -   Number of bits in quantized element.    int : 8 or 4
    qType   -   Type of quantization to perform.        Type::Symmetric or Type::Asymmetric
Function Arguments :
    local_output -  Pointer to shared memory to store quantized data.   int8_t*
    data         -  Pointer to input data.                              __half2*
    Params       -  Parameters for quantization.                        Params<qType, numBits>
*/
template <int numBits, Type qType>
DS_D_INLINE void _chunk(int8_t* local_output, const __half2* data, Params<qType, numBits> q_params);

/*
Helper function to do serial reduction on local memory.
Template Arguments :
    qType       -   Type of quantization to perform.        Type::Symmetric or Type::Asymmetric
    numChunks   -   Number of bits in quantized element.    int : 8 or 4
Function Arguments :
    local_buffer    -   Pointer memory with input half2 data to be quantized.
*/
template <Type qType, int numChunks>
DS_D_INLINE GroupStats<qType> _local_serial_reduce(__half2* local_buffer);

/*
The main loop of the kernel that quantizes array in local memory of __half2 type input data, when
Quantization parameters are pre-computed.
Template Arguments :
    qType       -   Type of quantization to perform.            Type::Symmetric or Type::Asymmetric
    numBits     -   Number of bits in quantized element.        int : 8 or 4
    numChunks   -   Number of chunks(16 bytes of Input data).   int : 8 or 4
Function Arguments :
    local_buffer    -   Pointer memory with input half2 data to be quantized.
    scales          -   Pointer to output scales.
    offsets         -   Pointer to output offsets.
    output_data     -   Pointer to output data.
    elems_per_group -   Number of elements to quantize in a group.
    q_parems        -   Quantization parameters.
*/
template <int numBits, Type qType, int numChunks>
DS_D_INLINE void local_array(cg::thread_block& tb,
                             cg::thread_block_tile<hw_warp_size>& warp,
                             __half2* local_buffer,
                             float* __restrict__ scales,
                             float* __restrict__ offsets,
                             int8_t* __restrict__ output_data,
                             const int& elems_per_group,
                             Params<qType, numBits> q_params);

/*
The main loop of the kernel that quantizes array in local memory of __half2 type input data.
This function computes quantization parameters for each group.
Template Arguments :
    qType   -   Type of quantization to perform.                Type::Symmetric or Type::Asymmetric
    numBits     -   Number of bits in quantized element.        int : 8 or 4
    numChunks   -   Number of chunks(16 bytes of Input data).   int : 8 or 4
Function Arguments :
    local_buffer    -   Pointer memory with input half2 data to be quantized.
    scales          -   Pointer to output scales.
    offsets         -   Pointer to output offsets.
    output_data     -   Pointer to output data.
    elems_per_group -   Number of elements to quantize in a group.
*/
template <Type qType, int numBits, int numChunks, int numWarps>
__device__ void local_array(__half2* local_buffer,
                            float* __restrict__ scales,
                            float* __restrict__ offsets,
                            int8_t* __restrict__ output_data,
                            const int& elems_per_group);

template <int numBits, Type qType>
DS_D_INLINE void _chunk(int8_t* local_output, const __half* data, Params<qType, numBits> q_params)
{
    constexpr int32_t elems = 16 / sizeof(__half);
    constexpr int32_t num_elems_packed = 8 / numBits;

#pragma unroll
    for (int i = 0, oi = 0; i < elems; i += num_elems_packed, oi++) {
        if (num_elems_packed == 1) {
            // TODO(cmikeh2): refactor to use conversion utils
            local_output[i] = q_params.quantize(data[i]);
        } else if (num_elems_packed == 2) {
            int8_t data_i8_1 = q_params.quantize(data[i]);
            int8_t data_i8_2 = q_params.quantize(data[i + 1]);
            auto data_i8 = PackedInt4{data_i8_2, data_i8_1};
            local_output[oi] = *((int8_t*)(&data_i8));
        }
    }
}

template <int numBits, Type qType>
DS_D_INLINE void _chunk(int8_t* local_output, const __half2* data, Params<qType, numBits> q_params)
{
    const __half* data_cast = reinterpret_cast<const __half*>(data);
    _chunk<numBits>(local_output, data_cast, q_params);
}

template <Type qType, int numChunks>
DS_D_INLINE GroupStats<qType> _local_serial_reduce(__half2* local_buffer)
{
    GroupStats<qType> stats;
#pragma unroll
    for (int i = 0; i < numChunks * h2_per_load; i++) { stats.update(local_buffer[i]); }

    return stats;
}

template <Type qType, int numBits, int numChunks>
DS_D_INLINE void local_array(cg::thread_block& tb,
                             cg::thread_block_tile<hw_warp_size>& warp,
                             __half2* local_buffer,
                             float* __restrict__ global_params,
                             int8_t* __restrict__ output_data,
                             const int& elems_per_group,
                             Params<qType, numBits> q_params)
{
    constexpr int num_ele_int8 = 8 / numBits;
    constexpr int num_int8_out = quantize::h_per_load / num_ele_int8;

    // Indexing offsets
    const int block_offset = tb.group_index().x * elems_per_group;
    const int elem_offset = tb.thread_index().x * quantize::h_per_load;
    const int base_offset = (block_offset + elem_offset) / num_ele_int8;
    const int stride = tb.size() * quantize::h_per_load / num_ele_int8;

    int8_t local_output[num_int8_out];

    if (tb.thread_index().x == 0) { q_params.store(global_params, tb.group_index().x); }

#pragma unroll
    for (int i = 0; i < numChunks; i++) {
        if (elem_offset + i * stride * num_ele_int8 < elems_per_group) {
            quantize::_chunk<numBits, qType>(
                local_output, local_buffer + i * quantize::h2_per_load, q_params);
            mem_access::store_global<num_int8_out>(output_data + (base_offset + i * stride),
                                                   local_output);
        }
    }
}

template <Type qType,
          int numBits,
          int numChunks,
          int numWarps = max_threads / hw_warp_size,
          int elemsPerBlock = 0>
__device__ void local_array(__half2* local_buffer,
                            float* __restrict__ global_params,
                            int8_t* __restrict__ output_data,
                            const int& elems_per_group)
{
    cg::thread_block tb = cg::this_thread_block();
    cg::thread_block_tile<hw_warp_size> warp = cg::tiled_partition<hw_warp_size>(tb);

    auto group_stats = _local_serial_reduce<qType, numChunks>(local_buffer);
    auto params = _get_params<qType, numBits, elemsPerBlock>(tb, warp, group_stats);

    quantize::local_array<qType, numBits, numChunks>(
        tb, warp, local_buffer, global_params, output_data, elems_per_group, params);
}

}  // namespace quantize