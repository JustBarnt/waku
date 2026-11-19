# Overlay of vcpkg's x64-windows-static-md: the same triplet, pinned to the VS 2022 toolset (v143,
# MSVC 14.44) so the packages match the compiler that builds waku. Only needed when more than one
# MSVC toolset is installed; see "More than one MSVC toolset" in docs/building.md.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PROVIDED_FORTRAN ON)
set(VCPKG_PLATFORM_TOOLSET v143)
set(VCPKG_PLATFORM_TOOLSET_VERSION 14.44)
