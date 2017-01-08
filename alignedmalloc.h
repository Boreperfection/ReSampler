#ifndef alignedmalloc_H
#define alignedmalloc_H

// alignedmalloc.h : for portable dynamic allocation of aligned memory

// usage:
// aligned_malloc(size, alignment)
// alignment must be a power-of-2
// returns a pointer to aligned allocated memory (or zero if unsuccessful)

// explanation: 
// _aligned_malloc() is Windows-specific
// other systems use posix_memalign()
// the function signatures also differ

#ifdef _MSC_VER 
	#include <malloc.h>
#else
	#include <stdlib.h>
#endif

inline void* aligned_malloc(size_t size, size_t alignment) {
	
	if (size == 0)
		return 0;
	
#ifdef _MSC_VER 
	return _aligned_malloc(size, alignment);
#else 
	void *memory;
	return posix_memalign(&memory, alignment, size) ? 0 : memory; // (note: posix_memalign returns 0 if successful, non-zero error code if not)
#endif

}

inline void aligned_free(void *ptr) {
#ifdef _MSC_VER 
	_aligned_free(ptr);
#else 
	free(ptr);
#endif
}

#endif