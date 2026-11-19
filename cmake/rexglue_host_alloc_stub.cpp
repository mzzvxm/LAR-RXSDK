#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define REX_HOST_ALLOC_SANITIZED 1
#endif
#endif

#if defined(__MINGW32__) && !defined(REX_HOST_ALLOC_SANITIZED)
// LARecomp branch. libc++.dll (llvm-mingw) allocates with its own operator new, and PE has no symbol
// interposition to make it call these forwarders: they would free blocks they did not allocate (std::string
// and std::vector code of a program frees what libc++.dll's compiled functions allocated). With MinGW every
// binary shares libc++.dll's operator new and delete instead, which is what the forwarders are for. The file
// compiles to nothing, as under ThreadSanitizer; cmake/rexglue_link_audit.cmake asks for the file only.
#define REX_HOST_ALLOC_SANITIZED 1
#endif

#ifndef REX_HOST_ALLOC_SANITIZED

#include <cstddef>
#include <new>

#include <rex/memory/host.h>

using rex::memory::detail::AllocType;
using rex::memory::detail::DeleteBlock;
using rex::memory::detail::NewBlock;

void* operator new(std::size_t size) {
  return NewBlock(size, 0, AllocType::kScalar, true);
}

void* operator new[](std::size_t size) {
  return NewBlock(size, 0, AllocType::kArray, true);
}

void* operator new(std::size_t size, std::align_val_t align) {
  return NewBlock(size, static_cast<std::size_t>(align), AllocType::kScalar, true);
}

void* operator new[](std::size_t size, std::align_val_t align) {
  return NewBlock(size, static_cast<std::size_t>(align), AllocType::kArray, true);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return NewBlock(size, 0, AllocType::kScalar, false);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return NewBlock(size, 0, AllocType::kArray, false);
}

void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
  return NewBlock(size, static_cast<std::size_t>(align), AllocType::kScalar, false);
}

void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
  return NewBlock(size, static_cast<std::size_t>(align), AllocType::kArray, false);
}

void operator delete(void* ptr) noexcept {
  DeleteBlock(ptr, 0, AllocType::kScalar);
}

void operator delete[](void* ptr) noexcept {
  DeleteBlock(ptr, 0, AllocType::kArray);
}

void operator delete(void* ptr, std::size_t size) noexcept {
  DeleteBlock(ptr, size, AllocType::kScalar);
}

void operator delete[](void* ptr, std::size_t size) noexcept {
  DeleteBlock(ptr, size, AllocType::kArray);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
  DeleteBlock(ptr, 0, AllocType::kScalar);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
  DeleteBlock(ptr, 0, AllocType::kArray);
}

void operator delete(void* ptr, std::size_t size, std::align_val_t) noexcept {
  DeleteBlock(ptr, size, AllocType::kScalar);
}

void operator delete[](void* ptr, std::size_t size, std::align_val_t) noexcept {
  DeleteBlock(ptr, size, AllocType::kArray);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  DeleteBlock(ptr, 0, AllocType::kScalar);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  DeleteBlock(ptr, 0, AllocType::kArray);
}

void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  DeleteBlock(ptr, 0, AllocType::kScalar);
}

void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  DeleteBlock(ptr, 0, AllocType::kArray);
}

#endif
