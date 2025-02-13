#pragma once
/// \file
///
/// Dynamically-resizable vector with fixed-capacity.
///
/// Copyright Gonzalo Brito Gadeschi 2015-2017
/// Copyright Eric Niebler 2013-2014
/// Copyright Casey Carter 2016
///
/// This file is released under the Boost Software License:
//
// Boost Software License - Version 1.0 - August 17th, 2003
//
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
//
// The copyright notices in the Software and this entire statement, including
// the above license grant, this restriction and the following disclaimer,
// must be included in all copies of the Software, in whole or in part, and
// all derivative works of the Software, unless such copies or derivative
// works are solely in the form of machine-executable object code generated by
// a source language processor.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
// SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
// FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// Some of the code has been adapted from the range-v3 library:
//
// https://github.com/ericniebler/range-v3/
//
// which is also under the Boost Software license.
//
// Some of the code has been adapted from libc++:
//
// and is annotated with "adapted from libc++" below, and is thus under the
// following license:
//
//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
#include <array>
#include <cstddef>     // for size_t
#include <cstdint>     // for fixed-width integer types
#include <cstdio>      // for assertion diagnostics
#include <functional>  // for less and equal_to
#include <iterator>    // for reverse_iterator and iterator traits
#include <limits>      // for numeric_limits
#include <stdexcept>   // for length_error
#include <type_traits> // for aligned_storage and all meta-functions

/// Optimizer allowed to assume that EXPR evaluates to true
#define SV_ASSUME(EXPR) static_cast<void>((EXPR) ? void(0) : __builtin_unreachable())

/// Assert pretty printer
#define SV_ASSERT(...)                                                                                                 \
	static_cast<void>((__VA_ARGS__)                                                                                    \
	                      ? void(0)                                                                                    \
	                      : ::deluge::sv_detail::assert_failure(static_cast<const char*>(__FILE__), __LINE__,          \
	                                                            "assertion failed: " #__VA_ARGS__))

/// Expect asserts the condition in debug builds and assumes the condition to be
/// true in release builds.
#if defined(NDEBUG) || 1
#define SV_EXPECT(EXPR) SV_ASSUME(EXPR)
#else
#define SV_EXPECT(EXPR) SV_ASSERT(EXPR)
#endif

namespace deluge {
// Private utilites (each std lib should already have this)
namespace sv_detail {
/// \name Utilities
///@{

template <class = void>
[[noreturn]] void assert_failure(char const* file, int line, char const* msg) {
	fprintf(stderr, "%s(%d): %s\n", file, line, msg);
	abort();
}

template <typename Rng>
using range_iterator_t = decltype(std::begin(std::declval<Rng>()));

template <typename T>
using iterator_reference_t = typename std::iterator_traits<T>::reference;

// clang-format off

/// Smallest fixed-width unsigned integer type that can represent
/// values in the range [0, N].
template <size_t N>
using smallest_size_t
	= std::conditional_t<(N < std::numeric_limits<uint8_t>::max()),  uint8_t,
		std::conditional_t<(N < std::numeric_limits<uint16_t>::max()), uint16_t,
		std::conditional_t<(N < std::numeric_limits<uint32_t>::max()), uint32_t,
		std::conditional_t<(N < std::numeric_limits<uint64_t>::max()), uint64_t,
						size_t>>>>;
// clang-format on

///@} // Utilities

/// Types implementing the `fixed_capactiy_vector`'s storage
namespace storage {
/// Storage for zero elements.
template <typename T>
struct zero_sized {
	using size_type = uint8_t;
	using value_type = T;
	using difference_type = ptrdiff_t;
	using pointer = T*;
	using const_pointer = T const*;

	/// Pointer to the data in the storage.
	static constexpr pointer data() noexcept { return nullptr; }
	/// Number of elements currently stored.
	static constexpr size_type size() noexcept { return 0; }
	/// Capacity of the storage.
	static constexpr size_type capacity() noexcept { return 0; }
	/// Is the storage empty?
	static constexpr bool empty() noexcept { return true; }
	/// Is the storage full?
	static constexpr bool full() noexcept { return true; }

	/// Constructs a new element at the end of the storage
	/// in-place.
	///
	/// Increases size of the storage by one.
	/// Always fails for empty storage.
	template <typename... Args>
	requires std::constructible_from<T, Args...>
	static constexpr void emplace_back(Args&&...) noexcept {
		SV_EXPECT(false && "tried to emplace_back on empty storage");
	}
	/// Removes the last element of the storage.
	/// Always fails for empty storage.
	static constexpr void pop_back() noexcept { SV_EXPECT(false && "tried to pop_back on empty storage"); }
	/// Changes the size of the storage without adding or
	/// removing elements (unsafe).
	///
	/// The size of an empty storage can only be changed to 0.
	static constexpr void unsafe_set_size(size_t new_size) noexcept {
		SV_EXPECT(new_size == 0
		          && "tried to change size of empty storage to "
		             "non-zero value");
	}

	/// Destroys all elements of the storage in range [begin,
	/// end) without changings its size (unsafe).
	///
	/// Nothing to destroy since the storage is empty.
	template <std::input_iterator InputIt>
	static constexpr void unsafe_destroy(InputIt /* begin */, InputIt /* end */) noexcept {}

	/// Destroys all elements of the storage without changing
	/// its size (unsafe).
	///
	/// Nothing to destroy since the storage is empty.
	static constexpr void unsafe_destroy_all() noexcept {}

	constexpr zero_sized() = default;
	constexpr zero_sized(zero_sized const&) = default;
	constexpr zero_sized& operator=(zero_sized const&) = default;
	constexpr zero_sized(zero_sized&&) = default;
	constexpr zero_sized& operator=(zero_sized&&) = default;
	~zero_sized() = default;

	/// Constructs an empty storage from an initializer list of
	/// zero elements.
	template <typename U>
	requires std::convertible_to<U, T>
	constexpr zero_sized(std::initializer_list<U> il) noexcept {
		SV_EXPECT(il.size() == 0
		          && "tried to construct storage::empty from a "
		             "non-empty initializer list");
	}
};

/// Storage for trivial types.
template <typename T, size_t Capacity>
struct trivial {
	static_assert(std::is_trivial_v<T>, "storage::trivial<T, C> requires std::is_trivial_v<T>");
	static_assert(Capacity != size_t{0}, "Capacity must be greater "
	                                     "than zero (use "
	                                     "storage::zero_sized instead)");

	using size_type = smallest_size_t<Capacity>;
	using value_type = T;
	using difference_type = ptrdiff_t;
	using pointer = T*;
	using const_pointer = T const*;

private:
	// If the value_type is const, make a const array of
	// non-const elements:
	using data_t = std::conditional_t<!std::is_const_v<T>, std::array<T, Capacity>,
	                                  const std::array<std::remove_const_t<T>, Capacity>>;
	alignas(alignof(T)) data_t data_{};

	/// Number of elements allocated in the storage:
	size_type size_ = 0;

public:
	/// Direct access to the underlying storage.
	///
	/// Complexity: O(1) in time and space.
	constexpr const_pointer data() const noexcept { return data_.data(); }

	/// Direct access to the underlying storage.
	///
	/// Complexity: O(1) in time and space.
	constexpr pointer data() noexcept { return data_.data(); }

	/// Number of elements in the storage.
	///
	/// Complexity: O(1) in time and space.
	constexpr size_type size() const noexcept { return size_; }

	/// Maximum number of elements that can be allocated in the
	/// storage.
	///
	/// Complexity: O(1) in time and space.
	static constexpr size_type capacity() noexcept { return Capacity; }

	/// Is the storage empty?
	constexpr bool empty() const noexcept { return size() == size_type{0}; }

	/// Is the storage full?
	constexpr bool full() const noexcept { return size() == Capacity; }

	/// Constructs an element in-place at the end of the
	/// storage.
	///
	/// Complexity: O(1) in time and space.
	/// Contract: the storage is not full.
	template <typename... Args>
	requires std::constructible_from<T, Args...> && std::assignable_from<value_type&, T>
	constexpr void emplace_back(Args&&... args) noexcept {
		SV_EXPECT(!full() && "tried to emplace_back on full storage!");
		data_[size_++] = T(std::forward<Args>(args)...);
	}

	/// Remove the last element from the container.
	///
	/// Complexity: O(1) in time and space.
	/// Contract: the storage is not empty.
	constexpr void pop_back() noexcept {
		SV_EXPECT(!empty() && "tried to pop_back from empty storage!");
		--size_;
	}

	/// (unsafe) Changes the container size to \p new_size.
	///
	/// Contract: `new_size <= capacity()`.
	/// \warning No elements are constructed or destroyed.
	constexpr void unsafe_set_size(size_t new_size) noexcept {
		SV_EXPECT(new_size <= Capacity && "new_size out-of-bounds [0, Capacity]");
		size_ = size_type(new_size);
	}

	/// (unsafe) Destroy elements in the range [begin, end).
	///
	/// \warning: The size of the storage is not changed.
	template <std::input_iterator InputIt>
	constexpr void unsafe_destroy(InputIt, InputIt) noexcept {}

	/// (unsafe) Destroys all elements of the storage.
	///
	/// \warning: The size of the storage is not changed.
	static constexpr void unsafe_destroy_all() noexcept {}

	constexpr trivial() noexcept = default;
	constexpr trivial(trivial const&) noexcept = default;
	constexpr trivial& operator=(trivial const&) noexcept = default;
	constexpr trivial(trivial&&) noexcept = default;
	constexpr trivial& operator=(trivial&&) noexcept = default;
	~trivial() = default;

private:
	template <std::convertible_to<T> U>
	static constexpr std::array<std::remove_const_t<T>, Capacity>
	unsafe_recast_init_list(std::initializer_list<U>& il) noexcept {
		SV_EXPECT(il.size() <= capacity()
		          && "trying to construct storage from an "
		             "initializer_list "
		             "whose size exceeds the storage capacity");
		std::array<std::remove_const_t<T>, Capacity> d_{};
		for (size_t i = 0, e = il.size(); i < e; ++i) {
			d_[i] = std::begin(il)[i];
		}
		return d_;
	}

public:
	/// Constructor from initializer list.
	///
	/// Contract: `il.size() <= capacity()`.
	template <std::convertible_to<T> U>
	constexpr trivial(std::initializer_list<U> il) noexcept : data_(unsafe_recast_init_list(il)) {
		unsafe_set_size(il.size());
	}
};

/// Storage for non-trivial elements.
template <typename T, size_t Capacity>
struct non_trivial {
	static_assert(!std::is_trivial_v<T>, "use storage::trivial for std::is_trivial_v<T> elements");
	static_assert(Capacity != size_t{0}, "Capacity must be greater than zero!");

	/// Smallest size_type that can represent Capacity:
	using size_type = smallest_size_t<Capacity>;
	using value_type = T;
	using difference_type = ptrdiff_t;
	using pointer = T*;
	using const_pointer = T const*;

private:
	/// Number of elements allocated in the embedded storage:
	size_type size_ = 0;

	//using aligned_storage_t = std::aligned_storage_t<sizeof(std::remove_const_t<T>), alignof(std::remove_const_t<T>)>;
	//using data_t = std::conditional_t<!std::is_const_v<T>, aligned_storage_t, const aligned_storage_t>;
	//alignas(alignof(T)) data_t data_[Capacity]{};
	// FIXME: ^ this won't work for types with "broken" alignof
	// like SIMD types (one would also need to provide an
	// overload of operator new to make heap allocations of this
	// type work for these types).

	// Kate's solution to deal with badly aligned members
	// This will be fixed with the GMA rework
	std::array<T, Capacity> data_{};

public:
	/// Direct access to the underlying storage.
	///
	/// Complexity: O(1) in time and space.
	const_pointer data() const noexcept { return data_.data(); }

	/// Direct access to the underlying storage.
	///
	/// Complexity: O(1) in time and space.
	pointer data() noexcept { return data_.data(); }

	/// Pointer to one-past-the-end.
	const_pointer end() const noexcept { return data() + size(); }

	/// Pointer to one-past-the-end.
	pointer end() noexcept { return data() + size(); }

	/// Number of elements in the storage.
	///
	/// Complexity: O(1) in time and space.
	constexpr size_type size() const noexcept { return size_; }

	/// Maximum number of elements that can be allocated in the
	/// storage.
	///
	/// Complexity: O(1) in time and space.
	static constexpr size_type capacity() noexcept { return Capacity; }

	/// Is the storage empty?
	[[nodiscard]] constexpr bool empty() const noexcept { return size() == size_type{0}; }

	/// Is the storage full?
	[[nodiscard]] constexpr bool full() const noexcept { return size() == Capacity; }

	/// Constructs an element in-place at the end of the
	/// embedded storage.
	///
	/// Complexity: O(1) in time and space.
	/// Contract: the storage is not full.
	template <typename... Args>
	requires std::constructible_from<T, Args...>
	void emplace_back(Args&&... args) noexcept(noexcept(new (end()) T(std::forward<Args>(args)...))) {
		SV_EXPECT(!full() && "tried to emplace_back on full storage");
		//new (end()) T(std::forward<Args>(args)...);
		//unsafe_set_size(size() + 1);

		// NOTE: (Kate) Faster somehow...
		void* end = &data_[size_++];
		new (end) T(std::forward<Args>(args)...);
	}

	/// Remove the last element from the container.
	///
	/// Complexity: O(1) in time and space.
	/// Contract: the storage is not empty.
	void pop_back() noexcept(std::is_nothrow_destructible_v<T>) {
		SV_EXPECT(!empty() && "tried to pop_back from empty storage!");

		// TODO:
		/// The below code commented out due to the change to std::array as a backing system for
		/// data_. Once a more robust allocator with proper alignment guarantees is used,
		/// we can switch back.

		// auto ptr = end() - 1;
		// ptr->~T();
		// unsafe_set_size(size() - 1);

		--size_;
	}

	/// (unsafe) Changes the container size to \p new_size.
	///
	/// Contract: `new_size <= capacity()`.
	/// \warning No elements are constructed or destroyed.
	constexpr void unsafe_set_size(size_t new_size) noexcept {
		SV_EXPECT(new_size <= Capacity && "new_size out-of-bounds [0, Capacity)");
		size_ = size_type(new_size);
	}

	/// (unsafe) Destroy elements in the range [begin, end).
	///
	/// \warning: The size of the storage is not changed.
	template <std::input_iterator InputIt>
	void unsafe_destroy(InputIt first, InputIt last) noexcept(std::is_nothrow_destructible_v<T>) {
		SV_EXPECT(first >= data() && first <= end() && "first is out-of-bounds");
		SV_EXPECT(last >= data() && last <= end() && "last is out-of-bounds");
		// for (; first != last; ++first) {
		// 	first->~T();
		// }
	}

	/// (unsafe) Destroys all elements of the storage.
	///
	/// \warning: The size of the storage is not changed.
	void unsafe_destroy_all() noexcept(std::is_nothrow_destructible_v<T>) {

		// TODO:
		/// The below code commented out due to the change to std::array as a backing system for
		/// data_. Once a more robust allocator with proper alignment guarantees is used,
		/// we can switch back.

		//	unsafe_destroy(data(), end());
	}

	constexpr non_trivial() = default;
	constexpr non_trivial(non_trivial const&) = default;
	constexpr non_trivial& operator=(non_trivial const&) = default;
	constexpr non_trivial(non_trivial&&) = default;
	constexpr non_trivial& operator=(non_trivial&&) = default;
	~non_trivial() noexcept(std::is_nothrow_destructible_v<T>) {
		// TODO:
		/// The below code commented out due to the change to std::array as a backing system for
		/// data_. Once a more robust allocator with proper alignment guarantees is used,
		/// we can switch back.

		//	 unsafe_destroy_all();
	}

	/// Constructor from initializer list.
	///
	/// Contract: `il.size() <= capacity()`.
	template <std::convertible_to<T> U>
	constexpr non_trivial(std::initializer_list<U> il) {
		SV_EXPECT(il.size() <= capacity()
		          && "trying to construct storage from an "
		             "initializer_list "
		             "whose size exceeds the storage capacity");
		std::copy(il.begin(), il.end(), data_.begin());
		unsafe_set_size(il.size());
	}
};

/// Selects the vector storage.
template <typename T, size_t Capacity>
using _t = std::conditional_t<Capacity == 0, zero_sized<T>,
                              std::conditional_t<std::is_trivial_v<T>, trivial<T, Capacity>, non_trivial<T, Capacity>>>;

} // namespace storage

} // namespace sv_detail

/// Dynamically-resizable fixed-capacity vector.
template <typename T, size_t Capacity>
struct static_vector : private sv_detail::storage::_t<T, Capacity> {
private:
	static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
	using base_t = sv_detail::storage::_t<T, Capacity>;
	using self = static_vector<T, Capacity>;

	using base_t::unsafe_destroy;
	using base_t::unsafe_destroy_all;
	using base_t::unsafe_set_size;

public:
	using value_type = typename base_t::value_type;
	using difference_type = ptrdiff_t;
	using reference = value_type&;
	using const_reference = value_type const&;
	using pointer = typename base_t::pointer;
	using const_pointer = typename base_t::const_pointer;
	using iterator = typename base_t::pointer;
	using const_iterator = typename base_t::const_pointer;
	using size_type = size_t;
	using reverse_iterator = ::std::reverse_iterator<iterator>;
	using const_reverse_iterator = ::std::reverse_iterator<const_iterator>;

	/// \name Size / capacity
	///@{
	using base_t::empty;
	using base_t::full;

	/// Number of elements in the vector
	[[nodiscard]] constexpr size_type size() const noexcept { return base_t::size(); }

	/// Maximum number of elements that can be allocated in the vector
	static constexpr size_type capacity() noexcept { return base_t::capacity(); }

	/// Maximum number of elements that can be allocated in the vector
	static constexpr size_type max_size() noexcept { return capacity(); }

	///@} // Size / capacity

	/// \name Data access
	///@{

	using base_t::data;

	///@} // Data access

	/// \name Iterators
	///@{

	constexpr iterator begin() noexcept { return data(); }
	constexpr const_iterator begin() const noexcept { return data(); }
	constexpr iterator end() noexcept { return data() + size(); }
	constexpr const_iterator end() const noexcept { return data() + size(); }

	reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
	const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
	reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
	const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

	constexpr const_iterator cbegin() noexcept { return begin(); }
	constexpr const_iterator cbegin() const noexcept { return begin(); }
	constexpr const_iterator cend() noexcept { return end(); }
	constexpr const_iterator cend() const noexcept { return end(); }

	///@}  // Iterators

private:
	/// \name Iterator bound-check utilites
	///@{

	template <typename It>
	constexpr void assert_iterator_in_range(It it) noexcept {
		static_assert(std::is_pointer_v<It>);
		SV_EXPECT(begin() <= it && "iterator not in range");
		SV_EXPECT(it <= end() && "iterator not in range");
	}

	template <typename It0, typename It1>
	constexpr void assert_valid_iterator_pair(It0 first, It1 last) noexcept {
		static_assert(std::is_pointer_v<It0>);
		static_assert(std::is_pointer_v<It1>);
		SV_EXPECT(first <= last && "invalid iterator pair");
	}

	template <typename It0, typename It1>
	constexpr void assert_iterator_pair_in_range(It0 first, It1 last) noexcept {
		assert_iterator_in_range(first);
		assert_iterator_in_range(last);
		assert_valid_iterator_pair(first, last);
	}

	///@}
public:
	/// \name Element access
	///
	///@{

	/// Unchecked access to element at index \p pos (UB if index not in
	/// range)
	constexpr reference operator[](size_type pos) noexcept { return std::begin(*this)[pos]; }

	/// Unchecked access to element at index \p pos (UB if index not in
	/// range)
	constexpr const_reference operator[](size_type pos) const noexcept { return std::begin(*this)[pos]; }

	/// Checked access to element at index \p pos (throws `out_of_range`
	/// if index not in range)
	constexpr reference at(size_type pos) noexcept { return std::begin(*this)[pos]; }

	/// Checked access to element at index \p pos (throws `out_of_range`
	/// if index not in range)
	constexpr const_reference at(size_type pos) const noexcept { return std::begin(*this)[pos]; }

	///
	constexpr reference front() noexcept { return std::begin(*this)[0]; }
	constexpr const_reference front() const noexcept { return std::begin(*this)[0]; }

	constexpr reference back() noexcept {
		SV_EXPECT(!empty() && "calling back on an empty vector");
		return std::begin(*this)[size() - 1];
	}
	constexpr const_reference back() const noexcept {
		SV_EXPECT(!empty() && "calling back on an empty vector");
		return std::begin(*this)[size() - 1];
	}

	///@} // Element access

	/// \name Modifiers
	///@{

	using base_t::emplace_back;
	using base_t::pop_back;

	/// Clears the vector.
	constexpr void clear() noexcept {
		unsafe_destroy_all();
		unsafe_set_size(0);
	}

	/// Appends \p value at the end of the vector.
	template <typename U>
	requires std::constructible_from<T, U> && std::assignable_from<reference, U&&>
	constexpr void push_back(U&& value) noexcept(noexcept(emplace_back(std::forward<U>(value)))) {
		SV_EXPECT(!full() && "vector is full!");
		emplace_back(std::forward<U>(value));
	}

	/// Appends a default constructed `T` at the end of the vector.

	void push_back() noexcept(
	    noexcept(emplace_back(T{}))) requires std::constructible_from<T, T> && std::assignable_from<reference, T&&> {
		SV_EXPECT(!full() && "vector is full!");
		emplace_back(T{});
	}

	template <typename... Args>
	requires std::constructible_from<T, Args...>
	constexpr iterator emplace(const_iterator position,
	                           Args&&... args) noexcept(noexcept(move_insert(position, std::declval<value_type*>(),
	                                                                         std::declval<value_type*>()))) {
		SV_EXPECT(!full() && "tried emplace on full static_vector!");
		assert_iterator_in_range(position);
		value_type a(std::forward<Args>(args)...);
		return move_insert(position, &a, &a + 1);
	}

	constexpr iterator insert(const_iterator position,
	                          const_reference x) noexcept(noexcept(insert(position, size_type(1),
	                                                                      x))) requires std::copy_constructible<T> {
		SV_EXPECT(!full() && "tried insert on full static_vector!");
		assert_iterator_in_range(position);
		return insert(position, size_type(1), x);
	}

	constexpr iterator
	insert(const_iterator position,
	       value_type&& x) noexcept(noexcept(move_insert(position, &x, &x + 1))) requires std::move_constructible<T> {
		SV_EXPECT(!full() && "tried insert on full static_vector!");
		assert_iterator_in_range(position);
		return move_insert(position, &x, &x + 1);
	}

	constexpr iterator insert(const_iterator position, size_type n,
	                          const T& x) noexcept(noexcept(push_back(x))) requires std::copy_constructible<T>

	{
		assert_iterator_in_range(position);
		const auto new_size = size() + n;
		SV_EXPECT(new_size <= capacity() && "trying to insert beyond capacity!");
		auto b = end();
		while (n != 0) {
			push_back(x);
			--n;
		}

		auto writable_position = begin() + (position - begin());
		std::rotate(writable_position, b, end());
		return writable_position;
	}

	template <class InputIt>
	requires std::input_iterator<InputIt> && std::constructible_from<value_type,
	                                                                 sv_detail::iterator_reference_t<InputIt>>
	constexpr iterator insert(const_iterator position, InputIt first,
	                          InputIt last) noexcept(noexcept(emplace_back(*first))) {
		assert_iterator_in_range(position);
		assert_valid_iterator_pair(first, last);
		if constexpr (std::random_access_iterator<InputIt>) {
			SV_EXPECT(size() + static_cast<size_type>(last - first) <= capacity()
			          && "trying to insert beyond capacity!");
		}
		auto b = end();

		// insert at the end and then just rotate:
		// cannot use try in constexpr function
		// try {  // if copy_constructor throws you get basic-guarantee?
		for (; first != last; ++first) {
			emplace_back(*first);
		}
		// } catch (...) {
		//   erase(b, end());
		//   throw;
		// }

		auto writable_position = begin() + (position - begin());
		std::rotate(writable_position, b, end());
		return writable_position;
	}

	template <std::input_iterator InputIt>
	constexpr iterator move_insert(const_iterator position, InputIt first,
	                               InputIt last) noexcept(noexcept(emplace_back(std::move(*first)))) {
		assert_iterator_in_range(position);
		assert_valid_iterator_pair(first, last);
		if constexpr (std::random_access_iterator<InputIt>) {
			SV_EXPECT(size() + static_cast<size_type>(last - first) <= capacity()
			          && "trying to insert beyond capacity!");
		}
		iterator b = end();

		// we insert at the end and then just rotate:
		for (; first != last; ++first) {
			emplace_back(std::move(*first));
		}
		auto writable_position = begin() + (position - begin());
		std::rotate<iterator>(writable_position, b, end());
		return writable_position;
	}

	constexpr iterator insert(const_iterator position, std::initializer_list<T> il) noexcept(
	    noexcept(insert(position, il.begin(), il.end()))) requires std::copy_constructible<T> {
		assert_iterator_in_range(position);
		return insert(position, il.begin(), il.end());
	}

	constexpr iterator erase(const_iterator position) noexcept requires std::movable<value_type> {
		assert_iterator_in_range(position);
		return erase(position, position + 1);
	}

	constexpr iterator erase(const_iterator first, const_iterator last) noexcept requires std::movable<value_type> {
		assert_iterator_pair_in_range(first, last);
		iterator p = begin() + (first - begin());
		if (first != last) {
			unsafe_destroy(std::move(p + (last - first), end(), p), end());
			unsafe_set_size(size() - static_cast<size_type>(last - first));
		}

		return p;
	}

	constexpr void
	swap(static_vector& other) noexcept(std::is_nothrow_swappable_v<T>) requires std::assignable_from<T&, T&&> {
		static_vector tmp = move(other);
		other = move(*this);
		(*this) = move(tmp);
	}

	/// Resizes the container to contain \p sz elements. If elements
	/// need to be appended, these are copy-constructed from \p value.
	///
	constexpr void
	resize(size_type sz,
	       T const& value) noexcept(std::is_nothrow_copy_constructible_v<T>) requires std::copy_constructible<T> {
		if (sz == size()) {
			return;
		}
		if (sz > size()) {
			SV_EXPECT(sz <= capacity()
			          && "static_vector cannot be resized to "
			             "a size greater than capacity");
			insert(end(), sz - size(), value);
		}
		else {
			erase(end() - (size() - sz), end());
		}
	}

private:
	constexpr void
	emplace_n(size_type n) noexcept((std::move_constructible<T> && std::is_nothrow_move_constructible_v<T>)
	                                || (std::copy_constructible<T> && std::is_nothrow_copy_constructible_v<T>)) requires
	    std::move_constructible<T> || std::copy_constructible<T> {
		SV_EXPECT(n <= capacity()
		          && "static_vector cannot be "
		             "resized to a size greater than "
		             "capacity");
		while (n != size()) {
			emplace_back(T{});
		}
	}

public:
	/// Resizes the container to contain \p sz elements. If elements
	/// need to be appended, these are move-constructed from `T{}` (or
	/// copy-constructed if `T` is not `std::move_constructible`).
	constexpr void resize(size_type sz) noexcept(
	    (std::move_constructible<T> && std::is_nothrow_move_constructible_v<T>)
	    || (std::copy_constructible<T> && std::is_nothrow_copy_constructible_v<T>)) requires std::movable<value_type> {
		if (sz == size()) {
			return;
		}

		if (sz > size()) {
			emplace_n(sz);
		}
		else {
			erase(end() - (size() - sz), end());
		}
	}

	///@}  // Modifiers

	/// \name Construct/copy/move/destroy
	///@{

	/// Default constructor.
	constexpr static_vector() = default;

	/// Copy constructor.
	constexpr static_vector(static_vector const& other) noexcept(
	    noexcept(insert(begin(), other.begin(), other.end()))) requires std::copy_constructible<value_type> {
		// nothin to assert: size of other cannot exceed capacity
		// because both vectors have the same type
		insert(begin(), other.begin(), other.end());
	}

	/// Move constructor.
	constexpr static_vector(static_vector&& other) noexcept(
	    noexcept(move_insert(begin(), other.begin(), other.end()))) requires std::move_constructible<value_type> {
		// nothin to assert: size of other cannot exceed capacity
		// because both vectors have the same type
		move_insert(begin(), other.begin(), other.end());
	}

	/// Copy assignment.
	constexpr static_vector& operator=(static_vector const& other) noexcept(noexcept(clear()) && noexcept(
	    insert(begin(), other.begin(), other.end()))) requires std::assignable_from<reference, const_reference> {
		// nothin to assert: size of other cannot exceed capacity
		// because both vectors have the same type
		clear();
		insert(this->begin(), other.begin(), other.end());
		return *this;
	}

	/// Move assignment.
	constexpr static_vector& operator=(static_vector&& other) noexcept(noexcept(clear()) and noexcept(
	    move_insert(begin(), other.begin(), other.end()))) requires std::move_constructible<value_type> {
		// nothin to assert: size of other cannot exceed capacity
		// because both vectors have the same type
		clear();
		move_insert(this->begin(), other.begin(), other.end());
		return *this;
	}

	/// Initializes vector with \p n default-constructed elements.
	explicit constexpr static_vector(size_type n) noexcept(
	    noexcept(emplace_n(n))) requires std::copy_constructible<T> || std::move_constructible<T> {
		SV_EXPECT(n <= capacity() && "size exceeds capacity");
		emplace_n(n);
	}

	/// Initializes vector with \p n with \p value.
	constexpr static_vector(size_type n,
	                        T const& value) noexcept(noexcept(insert(begin(), n,
	                                                                 value))) requires std::copy_constructible<T> {
		SV_EXPECT(n <= capacity() && "size exceeds capacity");
		insert(begin(), n, value);
	}

	/// Initialize vector from range [first, last).
	template <std::input_iterator InputIt>
	constexpr static_vector(InputIt first, InputIt last) {
		if constexpr (std::random_access_iterator<InputIt>) {
			SV_EXPECT(last - first >= 0);
			SV_EXPECT(static_cast<size_type>(last - first) <= capacity() && "range size exceeds capacity");
		}
		insert(begin(), first, last);
	}

	template <std::convertible_to<value_type> U>
	constexpr static_vector(std::initializer_list<U> il) noexcept(noexcept(base_t(std::move(il))))
	    : base_t(std::move(il)) { // assert happens in base_t constructor
	}

	constexpr static_vector(std::initializer_list<value_type> il) noexcept(noexcept(base_t(std::move(il))))
	    : base_t(std::move(il)) { // assert happens in base_t constructor
	}

	template <std::input_iterator InputIt>
	constexpr void assign(InputIt first,
	                      InputIt last) noexcept(noexcept(clear()) and noexcept(insert(begin(), first, last))) {
		if constexpr (std::random_access_iterator<InputIt>) {
			SV_EXPECT(last - first >= 0);
			SV_EXPECT(static_cast<size_type>(last - first) <= capacity() && "range size exceeds capacity");
		}
		clear();
		insert(begin(), first, last);
	}

	constexpr void assign(size_type n, const T& u) requires std::copy_constructible<T> {
		SV_EXPECT(n <= capacity() && "size exceeds capacity");
		clear();
		insert(begin(), n, u);
	}

	constexpr void assign(std::initializer_list<T> const& il) requires std::copy_constructible<T> {
		SV_EXPECT(il.size() <= capacity() && "initializer_list size exceeds capacity");
		clear();
		insert(this->begin(), il.begin(), il.end());
	}

	constexpr void assign(std::initializer_list<T>&& il) requires std::copy_constructible<T> {
		SV_EXPECT(il.size() <= capacity() && "initializer_list size exceeds capacity");
		clear();
		insert(this->begin(), il.begin(), il.end());
	}

	///@}  // Construct/copy/move/destroy/assign
};

template <typename T, size_t Capacity>
constexpr bool operator==(static_vector<T, Capacity> const& a, static_vector<T, Capacity> const& b) noexcept {
	return a.size() == b.size() and std::equal(a.begin(), a.end(), b.begin(), b.end(), std::equal_to<>{});
}

template <typename T, size_t Capacity>
constexpr bool operator<(static_vector<T, Capacity> const& a, static_vector<T, Capacity> const& b) noexcept {
	return std::equal(a.begin(), a.end(), b.begin(), b.end(), std::less<>{});
}

template <typename T, size_t Capacity>
constexpr bool operator!=(static_vector<T, Capacity> const& a, static_vector<T, Capacity> const& b) noexcept {
	return not(a == b);
}

template <typename T, size_t Capacity>
constexpr bool operator<=(static_vector<T, Capacity> const& a, static_vector<T, Capacity> const& b) noexcept {
	return std::equal(a.begin(), a.end(), b.begin(), b.end(), std::less_equal<>{});
}

template <typename T, size_t Capacity>
constexpr bool operator>(static_vector<T, Capacity> const& a, static_vector<T, Capacity> const& b) noexcept {
	return std::equal(a.begin(), a.end(), b.begin(), b.end(), std::greater<>{});
}

template <typename T, size_t Capacity>
constexpr bool operator>=(static_vector<T, Capacity> const& a, static_vector<T, Capacity> const& b) noexcept {
	return std::equal(a.begin(), a.end(), b.begin(), b.end(), std::greater_equal<>{});
}

namespace sv_detail {
template <class T, std::size_t N, std::size_t... I>
constexpr static_vector<std::remove_cv_t<T>, N> to_static_vector_impl(T (&a)[N], std::index_sequence<I...>) {
	return {{a[I]...}};
}

template <class T, std::size_t N, std::size_t... I>
constexpr static_vector<std::remove_cv_t<T>, N> to_static_vector_impl(T (&&a)[N], std::index_sequence<I...>) {
	return {{std::move(a[I])...}};
}
} // namespace sv_detail

template <class T, std::size_t N>
constexpr static_vector<std::remove_cv_t<T>, N> to_static_vector(T (&a)[N]) {
	return sv_detail::to_static_vector_impl(a, std::make_index_sequence<N>{});
}

template <class T, std::size_t N>
constexpr static_vector<std::remove_cv_t<T>, N> to_static_vector(T (&&a)[N]) {
	return sv_detail::to_static_vector_impl(std::move(a), std::make_index_sequence<N>{});
}

} // namespace deluge

// undefine all the internal macros
#undef SV_ASSUME
#undef SV_ASSERT
#undef SV_EXPECT
