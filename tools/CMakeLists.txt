add_executable(libatlas_tool
  asset_store.cpp
  libatlas_tool.cpp
)

target_compile_features(libatlas_tool PRIVATE cxx_std_17)
target_include_directories(libatlas_tool PRIVATE ${PROJECT_SOURCE_DIR}/third_party/picojson)
target_link_libraries(libatlas_tool PRIVATE libatlas)
