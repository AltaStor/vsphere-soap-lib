# CTest script (run via `cmake -P`): validates StreamOptimizedVmdkEncoder's
# output with a third-party reader. vmdk_encoder_writer writes a compressed
# streamOptimized VMDK plus the raw image it should decode to; qemu-img must
# accept it (`check`) and convert it back to exactly that raw image.
# A third-party reader is the point: grain directory pointers aimed at the
# GD/GT marker sector instead of one past it are what qemu-img rejects
# ("cluster offset for sector 0 points after EOF").
#
# Registered as DISABLED in tests/CMakeLists.txt when qemu-img isn't found
# on PATH, so this is a bonus interop check, not a hard requirement to run
# the suite.
#
# Invoked with -DWRITER=<path to vmdk_encoder_writer> -DQEMU_IMG=<path to qemu-img>
#              -DWORK_DIR=<scratch dir>

foreach(var WRITER QEMU_IMG WORK_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "vmdk_encoder_qemu_img_interop.cmake: ${var} not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(encoder_vmdk "${WORK_DIR}/encoder.vmdk")
set(expected_raw "${WORK_DIR}/expected.raw")
execute_process(
    COMMAND "${WRITER}" "${encoder_vmdk}" "${expected_raw}"
    RESULT_VARIABLE writer_result
    OUTPUT_VARIABLE writer_output
    ERROR_VARIABLE writer_error
)
if(NOT writer_result EQUAL 0)
    message(FATAL_ERROR "vmdk_encoder_qemu_img_interop: vmdk_encoder_writer failed:\n${writer_output}\n${writer_error}")
endif()

execute_process(
    COMMAND "${QEMU_IMG}" check -f vmdk "${encoder_vmdk}"
    RESULT_VARIABLE check_result
    OUTPUT_VARIABLE check_output
    ERROR_VARIABLE check_error
)
if(NOT check_result EQUAL 0)
    message(FATAL_ERROR "vmdk_encoder_qemu_img_interop: qemu-img check rejected the encoder's VMDK:\n${check_output}\n${check_error}")
endif()

set(roundtrip_raw "${WORK_DIR}/roundtrip.raw")
execute_process(
    COMMAND "${QEMU_IMG}" convert -f vmdk -O raw "${encoder_vmdk}" "${roundtrip_raw}"
    RESULT_VARIABLE convert_result
    OUTPUT_VARIABLE convert_output
    ERROR_VARIABLE convert_error
)
if(NOT convert_result EQUAL 0)
    message(FATAL_ERROR "vmdk_encoder_qemu_img_interop: qemu-img failed to read the encoder's VMDK:\n${convert_output}\n${convert_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${expected_raw}" "${roundtrip_raw}"
    RESULT_VARIABLE cmp_result
)
if(NOT cmp_result EQUAL 0)
    message(FATAL_ERROR "vmdk_encoder_qemu_img_interop: qemu-img's read of the encoder's VMDK doesn't match the expected raw image")
endif()
