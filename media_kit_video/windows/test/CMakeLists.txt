cmake_minimum_required(VERSION 3.14)
project(media_kit_windows_tests LANGUAGES CXX)
enable_testing()
add_executable(video_frame_size_test video_frame_size_test.cc)
target_compile_features(video_frame_size_test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(video_frame_size_test PRIVATE /W4 /WX /EHsc)
else()
  target_compile_options(video_frame_size_test PRIVATE -Wall -Wextra -Werror)
endif()

# Inject libmpv failures while compiling the actual output and manager code.
set(MEDIA_KIT_ANGLE_DIR "" CACHE PATH "ANGLE directory from the Flutter Windows build")
set(MEDIA_KIT_FLUTTER_DIR "" CACHE PATH "Flutter Windows ephemeral directory")
set(MEDIA_KIT_LIBMPV_DIR "" CACHE PATH "libmpv directory from the Flutter Windows build")
if(WIN32 AND MEDIA_KIT_ANGLE_DIR AND MEDIA_KIT_FLUTTER_DIR AND MEDIA_KIT_LIBMPV_DIR)
  add_executable(video_output_lifecycle_test video_output_lifecycle_test.cc
    ../video_output.cc ../video_output_manager.cc ../angle_surface_manager.cc ../utils.cc)
  target_compile_features(video_output_lifecycle_test PRIVATE cxx_std_17)
  target_compile_definitions(video_output_lifecycle_test PRIVATE
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR UNICODE _UNICODE)
  target_include_directories(video_output_lifecycle_test PRIVATE
    "${MEDIA_KIT_FLUTTER_DIR}" "${MEDIA_KIT_FLUTTER_DIR}/cpp_client_wrapper/include"
    "${MEDIA_KIT_LIBMPV_DIR}/include/mpv" "${MEDIA_KIT_ANGLE_DIR}/include")
  target_link_libraries(video_output_lifecycle_test PRIVATE
    "${MEDIA_KIT_ANGLE_DIR}/lib/libEGL.dll.lib" "${MEDIA_KIT_ANGLE_DIR}/lib/libGLESv2.dll.lib")
  file(GLOB ANGLE_RUNTIME "${MEDIA_KIT_ANGLE_DIR}/lib/*.dll")
  foreach(DLL IN LISTS ANGLE_RUNTIME)
    add_custom_command(TARGET video_output_lifecycle_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "$<TARGET_FILE_DIR:video_output_lifecycle_test>")
  endforeach()
  add_test(NAME video_output_lifecycle COMMAND video_output_lifecycle_test)
  set_tests_properties(video_output_lifecycle PROPERTIES TIMEOUT 30)
endif()
add_test(NAME video_frame_size COMMAND video_frame_size_test)

# Use the exact ANGLE archive extracted by the example build, not a system DLL.
if(WIN32 AND MEDIA_KIT_ANGLE_DIR)
  add_executable(texture_generation_test texture_generation_test.cc
    ../angle_surface_manager.cc ../utils.cc)
  target_compile_features(texture_generation_test PRIVATE cxx_std_17)
  target_compile_definitions(texture_generation_test PRIVATE
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR UNICODE _UNICODE)
  target_include_directories(texture_generation_test PRIVATE "${MEDIA_KIT_ANGLE_DIR}/include")
  target_link_libraries(texture_generation_test PRIVATE
    "${MEDIA_KIT_ANGLE_DIR}/lib/libEGL.dll.lib" "${MEDIA_KIT_ANGLE_DIR}/lib/libGLESv2.dll.lib")
  file(GLOB ANGLE_RUNTIME "${MEDIA_KIT_ANGLE_DIR}/lib/*.dll")
  foreach(DLL IN LISTS ANGLE_RUNTIME)
    add_custom_command(TARGET texture_generation_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "$<TARGET_FILE_DIR:texture_generation_test>")
  endforeach()
  add_test(NAME texture_generation COMMAND texture_generation_test)
  set_tests_properties(texture_generation PROPERTIES TIMEOUT 60)
endif()
