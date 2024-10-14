licenses(["restricted"])  # NVIDIA proprietary license

exports_files([
    "version.txt",
])
%{multiline_comment}
cc_import(
    name = "cusparse_shared_library",
    hdrs = [":headers"],
    shared_library = "lib/libcusparse.so.%{libcusparse_version}",
    deps = ["@cuda_nvjitlink//:nvjitlink"],
)
%{multiline_comment}
cc_library(
    name = "cusparse",
    %{comment}deps = [":cusparse_shared_library"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "headers",
    %{comment}hdrs = ["include/cusparse.h"],
    include_prefix = "third_party/gpus/cuda/include",
    includes = ["include"],
    strip_include_prefix = "include",
    visibility = ["@local_config_cuda//cuda:__pkg__"],
)
