//---------------------------------------------------------------------------
/*
	aligned_allocator - a C++ allocator that honors an explicit alignment

	Used for buffers that SIMD code loads/stores with an alignment assumption,
	e.g. the resample weight tables of visual/gl/ResampleImage.cpp declared as
	std::vector<float, aligned_allocator<float, 16> >.

	The alignment is a template parameter given in *bytes* (NAlign); the
	allocator guarantees at least max(NAlign, alignof(T)) and falls back to
	plain operator new when the ordinary allocation alignment is already
	sufficient, so over-aligned requests cost nothing extra.
*/
//---------------------------------------------------------------------------
#ifndef TVP_ALIGNED_ALLOCATOR_H
#define TVP_ALIGNED_ALLOCATOR_H

#include <cstddef>
#include <cstdlib>
#include <stdlib.h>	/* posix_memalign */
#include <new>
#include <limits>
#include <utility>	/* std::forward */

#if defined(_MSC_VER)
#include <malloc.h>
#endif

//---------------------------------------------------------------------------
/**
 * @brief	std::allocator replacement with a compile-time alignment
 * @param	T		element type
 * @param	NAlign	requested alignment in bytes (rounded up to a power of two
 *					that is a multiple of sizeof(void*) when it exceeds the
 *					alignment of ordinary allocations)
 */
template<typename T, int NAlign>
class aligned_allocator
{
public:
	typedef T				value_type;
	typedef T*				pointer;
	typedef const T*		const_pointer;
	typedef T&				reference;
	typedef const T&		const_reference;
	typedef std::size_t		size_type;
	typedef std::ptrdiff_t	difference_type;

	// std::allocator_traits cannot deduce the rebound type automatically: the
	// second template parameter of this class is a non-type parameter, which
	// does not match the "template<class U, class... Args> class Alloc"
	// pattern it rebinds through, so spell it out.
	template<typename U>
	struct rebind { typedef aligned_allocator<U, NAlign> other; };

	aligned_allocator() noexcept {}
	aligned_allocator(const aligned_allocator &) noexcept {}
	template<typename U>
	aligned_allocator(const aligned_allocator<U, NAlign> &) noexcept {}
	~aligned_allocator() noexcept {}

	pointer address(reference x) const { return &x; }
	const_pointer address(const_reference x) const { return &x; }

	size_type max_size() const
	{
		return (std::numeric_limits<size_type>::max)() / sizeof(T);
	}

	pointer allocate(size_type n, const void * = 0)
	{
		if(n > max_size()) throw std::bad_alloc();
		const size_type bytes = n * sizeof(T);
		if(bytes == 0) return 0;
		return static_cast<pointer>(Allocate(bytes));
	}

	void deallocate(pointer p, size_type)
	{
		Deallocate(p);
	}

	// explicit construct/destroy for C++03 containers; harmless under C++11,
	// where std::allocator_traits would otherwise synthesize them
	void construct(pointer p, const T & val) { ::new(static_cast<void*>(p)) T(val); }
	template<typename U, typename... Args>
	void construct(U * p, Args && ... args) { ::new(static_cast<void*>(p)) U(std::forward<Args>(args)...); }
	void destroy(pointer p) { p->~T(); }
	template<typename U>
	void destroy(U * p) { p->~U(); }

private:
	// Effective alignment: what the caller asked for, never less than what the
	// element type itself needs, and always usable for the underlying
	// allocator (power of two, multiple of sizeof(void*)).
	static size_type Alignment()
	{
		size_type a = NAlign > 0 ? static_cast<size_type>(NAlign)
								 : static_cast<size_type>(alignof(std::max_align_t));
		if(a < static_cast<size_type>(alignof(T))) a = static_cast<size_type>(alignof(T));
		size_type p = sizeof(void*);
		while(p < a) p <<= 1;
		return p;
	}

	static void * Allocate(size_type bytes)
	{
		const size_type align = Alignment();
		if(align <= static_cast<size_type>(alignof(std::max_align_t)))
		{
			// ordinary operator new already satisfies this alignment
			return ::operator new(bytes);
		}
#if defined(_MSC_VER)
		void * p = _aligned_malloc(bytes, align);
		if(!p) throw std::bad_alloc();
		return p;
#else
		void * p = 0;
		if(posix_memalign(&p, align, bytes) != 0) throw std::bad_alloc();
		return p;
#endif
	}

	static void Deallocate(void * p)
	{
		if(!p) return;
		const size_type align = Alignment();
		if(align <= static_cast<size_type>(alignof(std::max_align_t)))
		{
			::operator delete(p);
			return;
		}
#if defined(_MSC_VER)
		_aligned_free(p);
#else
		free(p);
#endif
	}
};

template<typename T, typename U, int NAlign>
inline bool operator==(const aligned_allocator<T, NAlign> &, const aligned_allocator<U, NAlign> &) noexcept
{
	return true;
}

template<typename T, typename U, int NAlign>
inline bool operator!=(const aligned_allocator<T, NAlign> &, const aligned_allocator<U, NAlign> &) noexcept
{
	return false;
}
//---------------------------------------------------------------------------

#endif
