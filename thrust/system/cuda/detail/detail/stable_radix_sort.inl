/*
 *  Copyright 2008-2012 NVIDIA Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <thrust/detail/config.h>

// do not attempt to compile this file with any other compiler
#if THRUST_DEVICE_COMPILER == THRUST_DEVICE_COMPILER_NVCC

#include <thrust/detail/copy.h>
#include <thrust/gather.h>
#include <thrust/sequence.h>
#include <thrust/iterator/iterator_traits.h>

#include <thrust/detail/temporary_array.h>
#include <thrust/detail/type_traits.h>
#include <thrust/detail/util/align.h>
#include <thrust/detail/raw_pointer_cast.h>
#include <thrust/system/system_error.h>
#include <thrust/system/cuda/error.h>


__THRUST_DISABLE_MSVC_POSSIBLE_LOSS_OF_DATA_WARNING_BEGIN


#include <thrust/system/cuda/detail/detail/b40c/radix_sort/enactor.cuh>
#include <thrust/system/cuda/detail/detail/b40c/util/multi_buffer.cuh>

namespace thrust
{
namespace system
{
namespace cuda
{
namespace detail
{
namespace detail
{

template<typename RandomAccessIterator>
void stable_radix_sort(RandomAccessIterator first,
                       RandomAccessIterator last)
{
    typedef typename thrust::iterator_system<RandomAccessIterator>::type system;
    typedef typename thrust::iterator_value<RandomAccessIterator>::type K;
    
    unsigned int num_elements = last - first;

    // ensure data is properly aligned
    if(!thrust::detail::util::is_aligned(thrust::raw_pointer_cast(&*first), 2*sizeof(K)))
    {
        thrust::detail::temporary_array<K, system> aligned_keys(first, last);
        stable_radix_sort(aligned_keys.begin(), aligned_keys.end());
        thrust::copy(aligned_keys.begin(), aligned_keys.end(), first);
        return;
    }
    
    __thrust_b40c::b40c::radix_sort::Enactor   sorter;
    __thrust_b40c::b40c::util::DoubleBuffer<K> double_buffer;
    
    // allocate temporary buffers
    thrust::detail::temporary_array<K,    system> temp_keys(num_elements);

    // hook up the double_buffer
    double_buffer.d_keys[double_buffer.selector]     = thrust::raw_pointer_cast(&*first);
    double_buffer.d_keys[double_buffer.selector ^ 1] = thrust::raw_pointer_cast(&temp_keys[0]);

    // note the value of the selector
    const int initial_selector = double_buffer.selector;

    // perform the sort
    cudaError_t error = sorter.Sort(double_buffer, num_elements);
    if(error)
    {
        throw thrust::system_error(error, thrust::cuda_category(), "stable_radix_sort: ");
    }
    
    // radix sort sometimes leaves results in the temporary buffer
    if(initial_selector != double_buffer.selector)
    {
        thrust::copy(temp_keys.begin(), temp_keys.end(), first);
    }
}

///////////////////////
// Key-Value Sorting //
///////////////////////

// sort values directly
template<typename RandomAccessIterator1,
         typename RandomAccessIterator2>
void stable_radix_sort_by_key(RandomAccessIterator1 first1,
                              RandomAccessIterator1 last1,
                              RandomAccessIterator2 first2,
                              thrust::detail::true_type)
{
    typedef typename thrust::iterator_system<RandomAccessIterator1>::type system;
    typedef typename thrust::iterator_value<RandomAccessIterator1>::type K;
    typedef typename thrust::iterator_value<RandomAccessIterator2>::type V;
    
    unsigned int num_elements = last1 - first1;

    // ensure data is properly aligned
    if(!thrust::detail::util::is_aligned(thrust::raw_pointer_cast(&*first1), 2*sizeof(K)))
    {
        thrust::detail::temporary_array<K,system> aligned_keys(first1, last1);
        stable_radix_sort_by_key(aligned_keys.begin(), aligned_keys.end(), first2);
        thrust::copy(aligned_keys.begin(), aligned_keys.end(), first1);
        return;
    }
    if(!thrust::detail::util::is_aligned(thrust::raw_pointer_cast(&*first2), 2*sizeof(V)))
    {
        thrust::detail::temporary_array<V,system> aligned_values(first2, first2 + num_elements);
        stable_radix_sort_by_key(first1, last1, aligned_values.begin());
        thrust::copy(aligned_values.begin(), aligned_values.end(), first2);
        return;
    }
   
    __thrust_b40c::b40c::radix_sort::Enactor     sorter;
    __thrust_b40c::b40c::util::DoubleBuffer<K,V> double_buffer;
    
    // allocate temporary buffers
    thrust::detail::temporary_array<K,    system> temp_keys(num_elements);
    thrust::detail::temporary_array<V,    system> temp_values(num_elements);

    // hook up the double_buffer
    double_buffer.d_keys[double_buffer.selector]       = thrust::raw_pointer_cast(&*first1);
    double_buffer.d_values[double_buffer.selector]     = thrust::raw_pointer_cast(&*first2);
    double_buffer.d_keys[double_buffer.selector ^ 1]   = thrust::raw_pointer_cast(&temp_keys[0]);
    double_buffer.d_values[double_buffer.selector ^ 1] = thrust::raw_pointer_cast(&temp_values[0]);

    // note the value of the selector
    const int initial_selector = double_buffer.selector;

    // perform the sort
    cudaError_t error = sorter.Sort(double_buffer, num_elements);
    if(error)
    {
        throw thrust::system_error(error, thrust::cuda_category(), "stable_radix_sort_by_key: ");
    }
    
    // radix sort sometimes leaves results in the temporary buffers
    if(initial_selector != double_buffer.selector)
    {
        thrust::copy(  temp_keys.begin(),   temp_keys.end(), first1);
        thrust::copy(temp_values.begin(), temp_values.end(), first2);
    }
}


// sort values indirectly
template<typename RandomAccessIterator1,
         typename RandomAccessIterator2>
void stable_radix_sort_by_key(RandomAccessIterator1 first1,
                              RandomAccessIterator1 last1,
                              RandomAccessIterator2 first2,
                              thrust::detail::false_type)
{
    typedef typename thrust::iterator_system<RandomAccessIterator1>::type system;
    typedef typename thrust::iterator_value<RandomAccessIterator2>::type V;
    
    unsigned int num_elements = last1 - first1;

    // sort with integer values and then permute the real values accordingly
    thrust::detail::temporary_array<unsigned int,system> permutation(num_elements);
    thrust::sequence(permutation.begin(), permutation.end());

    stable_radix_sort_by_key(first1, last1, permutation.begin());
    
    // copy values into temp vector and then permute
    thrust::detail::temporary_array<V,system> temp_values(first2, first2 + num_elements);
   
    // permute values
    thrust::gather(permutation.begin(), permutation.end(),
                   temp_values.begin(),
                   first2);
}


template<typename RandomAccessIterator1,
         typename RandomAccessIterator2>
void stable_radix_sort_by_key(RandomAccessIterator1 first1,
                              RandomAccessIterator1 last1,
                              RandomAccessIterator2 first2)
{
    typedef typename thrust::iterator_value<RandomAccessIterator2>::type V;

    // decide how to handle values
    static const bool sort_values_directly = thrust::detail::is_trivial_iterator<RandomAccessIterator2>::value &&
                                             thrust::detail::is_arithmetic<V>::value &&
                                             sizeof(V) <= 8;    // TODO profile this

    // XXX WAR unused variable warning
    (void) sort_values_directly;

    stable_radix_sort_by_key(first1, last1, first2, 
                             thrust::detail::integral_constant<bool, sort_values_directly>());
}

} // end namespace detail
} // end namespace detail
} // end namespace cuda
} // end namespace system
} // end namespace thrust


__THRUST_DISABLE_MSVC_POSSIBLE_LOSS_OF_DATA_WARNING_END


#endif // THRUST_DEVICE_COMPILER == THRUST_DEVICE_COMPILER_NVCC

