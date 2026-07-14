add_executable(redactly_inspect_model
    inspect_model.cpp
)

target_link_libraries(redactly_inspect_model PRIVATE onnxruntime::onnxruntime spdlog::spdlog)
redactly_harden(redactly_inspect_model)
