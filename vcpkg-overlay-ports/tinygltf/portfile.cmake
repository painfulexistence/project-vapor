# Header-only library.
#
# Overlay of the vcpkg-registry port pinned by the submodule: GitHub
# regenerated the on-the-fly archive for syoyo/tinygltf v2.9.7, so the
# tarball no longer matches the SHA512 recorded in the registry snapshot and
# every fresh `vcpkg install` fails. Fetching the tag's commit over git pins
# the exact tree by SHA and is immune to archive regeneration. Delete this
# overlay once the vcpkg submodule is bumped past the registry's fix.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/syoyo/tinygltf
    REF 488a70a3df62a4df1a736e9e56fb8836580c4888 # == refs/tags/v2.9.7
)

# Put the licence file where vcpkg expects it
# Copy the tinygltf header files and fix the path to json
vcpkg_replace_string("${SOURCE_PATH}/tiny_gltf.h" "#include \"json.hpp\"" "#include <nlohmann/json.hpp>")
file(INSTALL "${SOURCE_PATH}/tiny_gltf.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
