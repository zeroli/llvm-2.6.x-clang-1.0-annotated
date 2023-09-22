set(LLVM_LINK_COMPONENTS ${LLVM_TARGETS_TO_BUILD} core jit interpreter native)

add_llvm_example(Kaleidoscope
  toy.cpp
  )
