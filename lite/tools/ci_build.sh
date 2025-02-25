#!/bin/bash
set -ex

TESTS_FILE="./lite_tests.txt"
LIBS_FILE="./lite_libs.txt"


readonly ADB_WORK_DIR="/data/local/tmp"
readonly common_flags="-DWITH_LITE=ON -DLITE_WITH_LIGHT_WEIGHT_FRAMEWORK=OFF -DWITH_PYTHON=OFF -DWITH_TESTING=ON -DLITE_WITH_ARM=OFF"

readonly THIRDPARTY_TAR=https://paddle-inference-dist.bj.bcebos.com/PaddleLite/third-party-05b862.tar.gz
readonly workspace=$PWD

NUM_CORES_FOR_COMPILE=${LITE_BUILD_THREADS:-8}

function prepare_thirdparty {
    if [ ! -d $workspace/third-party -o -f $workspace/third-party-05b862.tar.gz ]; then
        rm -rf $workspace/third-party

        if [ ! -f $workspace/third-party-05b862.tar.gz ]; then
            wget $THIRDPARTY_TAR
        fi
        tar xzf third-party-05b862.tar.gz
    else
        git submodule update --init --recursive
    fi
}


# for code gen, a source file is generated after a test, but is dependended by some targets in cmake.
# here we fake an empty file to make cmake works.
function prepare_workspace {
    # in build directory
    # 1. Prepare gen_code file
    GEN_CODE_PATH_PREFIX=lite/gen_code
    mkdir -p ./${GEN_CODE_PATH_PREFIX}
    touch ./${GEN_CODE_PATH_PREFIX}/__generated_code__.cc

    # 2.Prepare debug tool
    DEBUG_TOOL_PATH_PREFIX=lite/tools/debug
    mkdir -p ./${DEBUG_TOOL_PATH_PREFIX}
    cp ../${DEBUG_TOOL_PATH_PREFIX}/analysis_tool.py ./${DEBUG_TOOL_PATH_PREFIX}/

    # clone submodule
    #git submodule update --init --recursive
    prepare_thirdparty
}

function check_need_ci {
    git log -1 --oneline | grep "test=develop" || exit -1
}

function cmake_x86 {
    prepare_workspace
    cmake ..  -DWITH_GPU=OFF -DWITH_MKLDNN=OFF -DLITE_WITH_X86=ON ${common_flags}
}

function cmake_opencl {
    prepare_workspace
    # $1: ARM_TARGET_OS in "android" , "armlinux"
    # $2: ARM_TARGET_ARCH_ABI in "armv8", "armv7" ,"armv7hf"
    # $3: ARM_TARGET_LANG in "gcc" "clang"
    cmake .. \
        -DLITE_WITH_OPENCL=ON \
        -DWITH_GPU=OFF \
        -DWITH_MKL=OFF \
        -DWITH_LITE=ON \
        -DLITE_WITH_CUDA=OFF \
        -DLITE_WITH_X86=OFF \
        -DLITE_WITH_ARM=ON \
        -DWITH_ARM_DOTPROD=ON   \
        -DLITE_WITH_LIGHT_WEIGHT_FRAMEWORK=ON \
        -DWITH_TESTING=ON \
        -DLITE_BUILD_EXTRA=ON \
        -DARM_TARGET_OS=$1 -DARM_TARGET_ARCH_ABI=$2 -DARM_TARGET_LANG=$3
}

function run_gen_code_test {
    local port=$1
    local gen_code_file_name="__generated_code__.cc"
    local gen_code_file_path="./lite/gen_code/${gen_code_file_path}"
    local adb_work_dir="/data/local/tmp"

    # 1. build test_cxx_api
    make test_cxx_api -j$NUM_CORES_FOR_COMPILE

    # 2. run test_cxx_api_lite in emulator to get opt model 
    local test_cxx_api_lite_path=$(find ./lite -name test_cxx_api)
    adb -s emulator-${port} push "./third_party/install/lite_naive_model" ${adb_work_dir}
    adb -s emulator-${port} push ${test_cxx_api_lite_path} ${adb_work_dir}
    adb -s emulator-${port} shell "${adb_work_dir}/test_cxx_api --model_dir=${adb_work_dir}/lite_naive_model --optimized_model=${adb_work_dir}/lite_naive_model_opt"

    # 3. build test_gen_code
    make test_gen_code -j$NUM_CORES_FOR_COMPILE

    # 4. run test_gen_code_lite in emulator to get __generated_code__.cc
    local test_gen_code_lite_path=$(find ./lite -name test_gen_code)
    adb -s emulator-${port} push ${test_gen_code_lite_path} ${adb_work_dir}
    adb -s emulator-${port} shell "${adb_work_dir}/test_gen_code --optimized_model=${adb_work_dir}/lite_naive_model_opt --generated_code_file=${adb_work_dir}/${gen_code_file_name}"

    # 5. pull __generated_code__.cc down and mv to build real path
    adb -s emulator-${port} pull "${adb_work_dir}/${gen_code_file_name}" .
    mv ${gen_code_file_name} ${gen_code_file_path}

    # 6. build test_generated_code
    make test_generated_code -j$NUM_CORES_FOR_COMPILE
}

# $1: ARM_TARGET_OS in "android" , "armlinux"
# $2: ARM_TARGET_ARCH_ABI in "armv8", "armv7" ,"armv7hf"
# $3: ARM_TARGET_LANG in "gcc" "clang"
function build_opencl {
    os=$1
    abi=$2
    lang=$3

    cur_dir=$(pwd)
    if [[ ${os} == "armlinux" ]]; then
        # TODO(hongming): enable compile armv7 and armv7hf on armlinux, and clang compile
        if [[ ${lang} == "clang" ]]; then
            echo "clang is not enabled on armlinux yet"
            return 0
        fi
        if [[ ${abi} == "armv7hf" ]]; then
            echo "armv7hf is not supported on armlinux yet"
            return 0
        fi
        if [[ ${abi} == "armv7" ]]; then
            echo "armv7 is not supported on armlinux yet"
            return 0
        fi
    fi

    if [[ ${os} == "android" && ${abi} == "armv7hf" ]]; then
        echo "android do not need armv7hf"
        return 0
    fi

    build_dir=$cur_dir/build.lite.${os}.${abi}.${lang}.opencl
    mkdir -p $build_dir
    cd $build_dir

    cmake_opencl ${os} ${abi} ${lang}
    make opencl_clhpp
    build $TESTS_FILE

    # test publish inference lib
    make publish_inference
}

# This method is only called in CI.
function cmake_x86_for_CI {
    prepare_workspace # fake an empty __generated_code__.cc to pass cmake.
    cmake ..  -DWITH_GPU=OFF -DWITH_MKLDNN=OFF -DLITE_WITH_X86=ON ${common_flags} -DLITE_WITH_PROFILE=ON -DWITH_MKL=OFF \
        -DLITE_BUILD_EXTRA=ON \

    # Compile and execute the gen_code related test, so it will generate some code, and make the compilation reasonable.
    # make test_gen_code -j$NUM_CORES_FOR_COMPILE
    # make test_cxx_api -j$NUM_CORES_FOR_COMPILE
    # ctest -R test_cxx_api
    # ctest -R test_gen_code
    # make test_generated_code -j$NUM_CORES_FOR_COMPILE
}

function cmake_gpu {
    prepare_workspace
    cmake .. " -DWITH_GPU=ON {common_flags} -DLITE_WITH_GPU=ON"
}

function check_style {
    export PATH=/usr/bin:$PATH
    #pre-commit install
    clang-format --version

    if ! pre-commit run -a ; then
        git diff
        exit 1
    fi
}

function build_single {
    #make $1 -j$(expr $(nproc) - 2)
    make $1 -j$NUM_CORES_FOR_COMPILE
}

function build {
    make lite_compile_deps -j$NUM_CORES_FOR_COMPILE

    # test publish inference lib
    # make publish_inference
}

# It will eagerly test all lite related unittests.
function test_server {
    # Due to the missing of x86 kernels, we skip the following tests temporarily.
    # TODO(xxx) clear the skip list latter
    local skip_list=("test_paddle_api" "test_cxx_api" "test_googlenet"
                     "test_mobilenetv1_lite_x86" "test_mobilenetv2_lite_x86"
                     "test_inceptionv4_lite_x86" "test_light_api"
                     "test_apis" "test_model_bin"
                    )
    local to_skip=0
    for _test in $(cat $TESTS_FILE); do
        to_skip=0
        for skip_name in ${skip_list[@]}; do
            if [ $skip_name = $_test ]; then
                echo "to skip " $skip_name
                to_skip=1
            fi
        done

        if [ $to_skip -eq 0 ]; then
            ctest -R $_test -V
        fi
    done
}

# Build the code and run lite server tests. This is executed in the CI system.
function build_test_server {
    mkdir -p ./build
    cd ./build
    export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/paddle/build/third_party/install/mklml/lib"
    cmake_x86_for_CI
    build

    test_server
}

function build_test_train {
    mkdir -p ./build
    cd ./build
    export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/paddle/build/third_party/install/mklml/lib"
    prepare_workspace # fake an empty __generated_code__.cc to pass cmake.
    cmake .. -DWITH_LITE=ON -DWITH_GPU=OFF -DWITH_PYTHON=ON -DLITE_WITH_X86=ON -DLITE_WITH_LIGHT_WEIGHT_FRAMEWORK=OFF -DWITH_TESTING=ON -DWITH_MKL=OFF \
        -DLITE_BUILD_EXTRA=ON \

    make test_gen_code -j$NUM_CORES_FOR_COMPILE
    make test_cxx_api -j$NUM_CORES_FOR_COMPILE
    ctest -R test_cxx_api
    ctest -R test_gen_code
    make test_generated_code -j$NUM_CORES_FOR_COMPILE

    make -j$NUM_CORES_FOR_COMPILE

    find -name "*.whl" | xargs pip2 install
    python ../lite/tools/python/lite_test.py

}

# test_arm_android <some_test_name> <adb_port_number>
function test_arm_android {
    local test_name=$1
    local port=$2
    if [[ "${test_name}x" == "x" ]]; then
        echo "test_name can not be empty"
        exit 1
    fi
    if [[ "${port}x" == "x" ]]; then
        echo "Port can not be empty"
        exit 1
    fi

    echo "test name: ${test_name}"
    adb_work_dir="/data/local/tmp"

    skip_list=("test_model_parser" "test_mobilenetv1" "test_mobilenetv2" "test_resnet50" "test_inceptionv4" "test_light_api" "test_apis" "test_paddle_api" "test_cxx_api" "test_gen_code" "test_mobilenetv1_int8" "test_subgraph_pass")
    for skip_name in ${skip_list[@]} ; do
        [[ $skip_name =~ (^|[[:space:]])$test_name($|[[:space:]]) ]] && echo "skip $test_name" && return
    done

    local testpath=$(find ./lite -name ${test_name})

    adb -s emulator-${port} push ${testpath} ${adb_work_dir}
    adb -s emulator-${port} shell "cd ${adb_work_dir} && ./${test_name}"
    adb -s emulator-${port} shell "rm ${adb_work_dir}/${test_name}"
}

# test_npu <some_test_name> <adb_port_number>
function test_npu {
    local test_name=$1
    local port=$2
    if [[ "${test_name}x" == "x" ]]; then
        echo "test_name can not be empty"
        exit 1
    fi
    if [[ "${port}x" == "x" ]]; then
        echo "Port can not be empty"
        exit 1
    fi

    echo "test name: ${test_name}"
    adb_work_dir="/data/local/tmp"

    skip_list=("test_model_parser" "test_mobilenetv1" "test_mobilenetv2" "test_resnet50" "test_inceptionv4" "test_light_api" "test_apis" "test_paddle_api" "test_cxx_api" "test_gen_code")
    for skip_name in ${skip_list[@]} ; do
        [[ $skip_name =~ (^|[[:space:]])$test_name($|[[:space:]]) ]] && echo "skip $test_name" && return
    done

    local testpath=$(find ./lite -name ${test_name})

    # note the ai_ddk_lib is under paddle-lite root directory
    adb -s emulator-${port} push ../ai_ddk_lib/lib64/* ${adb_work_dir}
    adb -s emulator-${port} push ${testpath} ${adb_work_dir}

    if [[ ${test_name} == "test_npu_pass" ]]; then
        local model_name=mobilenet_v1
        adb -s emulator-${port} push "./third_party/install/${model_name}" ${adb_work_dir}
        adb -s emulator-${port} shell "rm -rf ${adb_work_dir}/${model_name}_opt "
        adb -s emulator-${port} shell "cd ${adb_work_dir}; export LD_LIBRARY_PATH=./ ; export GLOG_v=0; ./${test_name} --model_dir=./${model_name} --optimized_model=./${model_name}_opt"
    elif [[ ${test_name} == "test_subgraph_pass" ]]; then
        local model_name=mobilenet_v1
        adb -s emulator-${port} push "./third_party/install/${model_name}" ${adb_work_dir}
        adb -s emulator-${port} shell "cd ${adb_work_dir}; export LD_LIBRARY_PATH=./ ; export GLOG_v=0; ./${test_name} --model_dir=./${model_name}"
    else
        adb -s emulator-${port} shell "cd ${adb_work_dir}; export LD_LIBRARY_PATH=./ ; ./${test_name}"
    fi
}

function test_npu_model {
    local test_name=$1
    local port=$2
    local model_dir=$3

    if [[ "${test_name}x" == "x" ]]; then
        echo "test_name can not be empty"
        exit 1
    fi
    if [[ "${port}x" == "x" ]]; then
        echo "Port can not be empty"
        exit 1
    fi
    if [[ "${model_dir}x" == "x" ]]; then
        echo "Model dir can not be empty"
        exit 1
    fi

    echo "test name: ${test_name}"
    adb_work_dir="/data/local/tmp"

    testpath=$(find ./lite -name ${test_name})
    adb -s emulator-${port} push ../ai_ddk_lib/lib64/* ${adb_work_dir}
    adb -s emulator-${port} push ${model_dir} ${adb_work_dir}
    adb -s emulator-${port} push ${testpath} ${adb_work_dir}
    adb -s emulator-${port} shell chmod +x "${adb_work_dir}/${test_name}"
    local adb_model_path="${adb_work_dir}/`basename ${model_dir}`"
    adb -s emulator-${port} shell "export LD_LIBRARY_PATH=${adb_work_dir}; ${adb_work_dir}/${test_name} --model_dir=$adb_model_path"
}

# test the inference high level api
function test_arm_api {
    local port=$1
    local test_name="test_paddle_api"

    make $test_name -j$NUM_CORES_FOR_COMPILE

    local model_path=$(find . -name "lite_naive_model")
    local remote_model=${adb_work_dir}/paddle_api
    local testpath=$(find ./lite -name ${test_name})

    arm_push_necessary_file $port $model_path $remote_model
    adb -s emulator-${port} shell mkdir -p $remote_model
    adb -s emulator-${port} push ${testpath} ${adb_work_dir}
    adb -s emulator-${port} shell chmod +x "${adb_work_dir}/${test_name}"
    adb -s emulator-${port} shell "${adb_work_dir}/${test_name} --model_dir $remote_model"
}

function test_arm_model {
    local test_name=$1
    local port=$2
    local model_dir=$3

    if [[ "${test_name}x" == "x" ]]; then
        echo "test_name can not be empty"
        exit 1
    fi
    if [[ "${port}x" == "x" ]]; then
        echo "Port can not be empty"
        exit 1
    fi
    if [[ "${model_dir}x" == "x" ]]; then
        echo "Model dir can not be empty"
        exit 1
    fi

    echo "test name: ${test_name}"
    adb_work_dir="/data/local/tmp"

    testpath=$(find ./lite -name ${test_name})
    adb -s emulator-${port} push ${model_dir} ${adb_work_dir}
    adb -s emulator-${port} push ${testpath} ${adb_work_dir}
    adb -s emulator-${port} shell chmod +x "${adb_work_dir}/${test_name}"
    local adb_model_path="${adb_work_dir}/`basename ${model_dir}`"
    adb -s emulator-${port} shell "${adb_work_dir}/${test_name} --model_dir=$adb_model_path"
}

function _test_model_optimize_tool {
    local port=$1
    local remote_model_path=$ADB_WORK_DIR/lite_naive_model
    local remote_test=$ADB_WORK_DIR/model_optimize_tool
    local adb="adb -s emulator-${port}"

    make model_optimize_tool -j$NUM_CORES_FOR_COMPILE
    local test_path=$(find . -name model_optimize_tool | head -n1)
    local model_path=$(find . -name lite_naive_model | head -n1)
    $adb push ${test_path} ${ADB_WORK_DIR}
    $adb shell mkdir -p $remote_model_path
    $adb push $model_path/* $remote_model_path
    $adb shell $remote_test --model_dir $remote_model_path --optimize_out ${remote_model_path}.opt \
         --valid_targets "arm"
}

function _test_paddle_code_generator {
    local port=$1
    local test_name=paddle_code_generator
    local remote_test=$ADB_WORK_DIR/$test_name
    local remote_model=$ADB_WORK_DIR/lite_naive_model.opt
    local adb="adb -s emulator-${port}"

    make paddle_code_generator -j$NUM_CORES_FOR_COMPILE
    local test_path=$(find . -name $test_name | head -n1)

    $adb push $test_path $remote_test
    $adb shell $remote_test --optimized_model $remote_model --generated_code_file $ADB_WORK_DIR/gen_code.cc
}

function cmake_npu {
    prepare_workspace
    # $1: ARM_TARGET_OS in "android" , "armlinux"
    # $2: ARM_TARGET_ARCH_ABI in "armv8", "armv7" ,"armv7hf"
    # $3: ARM_TARGET_LANG in "gcc" "clang"

    # NPU libs need API LEVEL 24 above
    build_dir=`pwd`

    cmake .. \
        -DWITH_GPU=OFF \
        -DWITH_MKL=OFF \
        -DWITH_LITE=ON \
        -DLITE_WITH_CUDA=OFF \
        -DLITE_WITH_X86=OFF \
        -DLITE_WITH_ARM=ON \
        -DWITH_ARM_DOTPROD=ON   \
        -DLITE_WITH_LIGHT_WEIGHT_FRAMEWORK=ON \
        -DWITH_TESTING=ON \
        -DLITE_WITH_NPU=ON \
        -DANDROID_API_LEVEL=24 \
        -DLITE_BUILD_EXTRA=ON \
        -DNPU_DDK_ROOT="${build_dir}/../ai_ddk_lib/" \
        -DARM_TARGET_OS=$1 -DARM_TARGET_ARCH_ABI=$2 -DARM_TARGET_LANG=$3
}

function cmake_arm {
    prepare_workspace
    # $1: ARM_TARGET_OS in "android" , "armlinux"
    # $2: ARM_TARGET_ARCH_ABI in "armv8", "armv7" ,"armv7hf"
    # $3: ARM_TARGET_LANG in "gcc" "clang"
    cmake .. \
        -DWITH_GPU=OFF \
        -DWITH_MKL=OFF \
        -DWITH_LITE=ON \
        -DLITE_WITH_CUDA=OFF \
        -DLITE_WITH_X86=OFF \
        -DLITE_WITH_ARM=ON \
        -DWITH_ARM_DOTPROD=ON   \
        -DLITE_WITH_LIGHT_WEIGHT_FRAMEWORK=ON \
        -DWITH_TESTING=ON \
        -DLITE_BUILD_EXTRA=ON \
        -DARM_TARGET_OS=$1 -DARM_TARGET_ARCH_ABI=$2 -DARM_TARGET_LANG=$3
}

# $1: ARM_TARGET_OS in "android" , "armlinux"
# $2: ARM_TARGET_ARCH_ABI in "armv8", "armv7" ,"armv7hf"
# $3: ARM_TARGET_LANG in "gcc" "clang"
function build_arm {
    os=$1
    abi=$2
    lang=$3

    cur_dir=$(pwd)
    # TODO(xxx): enable armlinux clang compile
    if [[ ${os} == "armlinux" && ${lang} == "clang" ]]; then
        echo "clang is not enabled on armlinux yet"
        return 0
    fi

    if [[ ${os} == "android" && ${abi} == "armv7hf" ]]; then
        echo "android do not need armv7hf"
        return 0
    fi

    build_dir=$cur_dir/build.lite.${os}.${abi}.${lang}
    mkdir -p $build_dir
    cd $build_dir

    cmake_arm ${os} ${abi} ${lang}
    build $TESTS_FILE

    # test publish inference lib
    make publish_inference
}

# $1: ARM_TARGET_OS in "android"
# $2: ARM_TARGET_ARCH_ABI in "armv8", "armv7"
# $3: ARM_TARGET_LANG in "gcc" "clang"
# $4: test_name
function build_npu {
    os=$1
    abi=$2
    lang=$3
    local test_name=$4

    cur_dir=$(pwd)

    build_dir=$cur_dir/build.lite.npu.${os}.${abi}.${lang}
    mkdir -p $build_dir
    cd $build_dir

    cmake_npu ${os} ${abi} ${lang}

    if [[ "${test_name}x" != "x" ]]; then
        build_single $test_name
    else
        build $TESTS_FILE
    fi
}

# $1: ARM_TARGET_OS in "android" , "armlinux"
# $2: ARM_TARGET_ARCH_ABI in "armv8", "armv7" ,"armv7hf"
# $3: ARM_TARGET_LANG in "gcc" "clang"
# $4: android test port
# Note: test must be in build dir
function test_arm {
    os=$1
    abi=$2
    lang=$3
    port=$4

    if [[ ${os} == "armlinux" ]]; then
        # TODO(hongming): enable test armlinux on armv8, armv7 and armv7hf
        echo "Skip test arm linux yet. armlinux must in another docker"
        return 0
    fi

    if [[ ${os} == "android" && ${abi} == "armv7hf" ]]; then
        echo "android do not need armv7hf"
        return 0
    fi

    # prepare for CXXApi test
    local adb="adb -s emulator-${port}"
    $adb shell mkdir -p /data/local/tmp/lite_naive_model_opt

    echo "test file: ${TESTS_FILE}"
    for _test in $(cat $TESTS_FILE); do
        test_arm_android $_test $port
    done

    # test finally
    test_arm_api $port

    _test_model_optimize_tool $port
    _test_paddle_code_generator $port
}

function prepare_emulator {
    local port_armv8=$1
    local port_armv7=$2

    adb kill-server
    adb devices | grep emulator | cut -f1 | while read line; do adb -s $line emu kill; done
    # start android armv8 and armv7 emulators first
    echo n | avdmanager create avd -f -n paddle-armv8 -k "system-images;android-24;google_apis;arm64-v8a"
    echo -ne '\n' | ${ANDROID_HOME}/emulator/emulator -avd paddle-armv8 -noaudio -no-window -gpu off -port ${port_armv8} &
    sleep 1m
    if [[ "${port_armv7}x" != "x" ]]; then
        echo n | avdmanager create avd -f -n paddle-armv7 -k "system-images;android-24;google_apis;armeabi-v7a"
        echo -ne '\n' | ${ANDROID_HOME}/emulator/emulator -avd paddle-armv7 -noaudio -no-window -gpu off -port ${port_armv7} &
        sleep 1m
    fi
}

function arm_push_necessary_file {
    local port=$1
    local testpath=$2
    local adb_work_dir=$3

    adb -s emulator-${port} push ${testpath} ${adb_work_dir}
}

function build_test_arm_opencl {
    ########################################################################
    cur=$PWD

    # job 1
    build_opencl "android" "armv8" "gcc"
    cd $cur

    # job 2
    build_opencl "android" "armv7" "gcc"
    cd $cur

    echo "Done"
}

# We split the arm unittest into several sub-tasks to parallel and reduce the overall CI timetime.
# sub-task1
function build_test_arm_subtask_android {
    ########################################################################
    # job 1-4 must be in one runner
    port_armv8=5554
    port_armv7=5556

    prepare_emulator $port_armv8 $port_armv7

    # job 1
    build_arm "android" "armv8" "gcc"
    run_gen_code_test ${port_armv8}
    test_arm "android" "armv8" "gcc" ${port_armv8}
    cd -

    # job 2
    #build_arm "android" "armv8" "clang"
    #run_gen_code_test ${port_armv8}
    #test_arm "android" "armv8" "clang" ${port_armv8}
    #cd -

    # job 3
    build_arm "android" "armv7" "gcc"
    run_gen_code_test ${port_armv7}
    test_arm "android" "armv7" "gcc" ${port_armv7}
    cd -

    # job 4
    #build_arm "android" "armv7" "clang"
    #run_gen_code_test ${port_armv7}
    #test_arm "android" "armv7" "clang" ${port_armv7}
    #cd -

    adb devices | grep emulator | cut -f1 | while read line; do adb -s $line emu kill; done
    echo "Done"
}

# sub-task2
function build_test_arm_subtask_armlinux {
    cur=$PWD
    # job 5
    build_arm "armlinux" "armv8" "gcc"
    test_arm "armlinux" "armv8" "gcc" $port_armv8
    cd $cur

    # job 6
    build_arm "armlinux" "armv7" "gcc"
    test_arm "armlinux" "armv7" "gcc" $port_armv8
    cd $cur

    # job 7
    build_arm "armlinux" "armv7hf" "gcc"
    test_arm "armlinux" "armv7hf" "gcc" $port_armv8
    cd $cur

    echo "Done"
}

# sub-task-model
function build_test_arm_subtask_model {
    local port_armv8=5554
    local port_armv7=5556
    # We just test following single one environment to limit the CI time.
    local os=android
    local abi=armv8
    local lang=gcc

    local test_name=$1
    local model_name=$2

    cur_dir=$(pwd)
    build_dir=$cur_dir/build.lite.${os}.${abi}.${lang}
    mkdir -p $build_dir
    cd $build_dir
    cmake_arm $os $abi $lang
    make $test_name -j$NUM_CORES_FOR_COMPILE

    prepare_emulator $port_armv8 $port_armv7

    # just test the model on armv8
    test_arm_model $test_name $port_armv8 "./third_party/install/$model_name"

    adb devices | grep emulator | cut -f1 | while read line; do adb -s $line emu kill; done
    echo "Done"
    cd -
    rm -rf $build_dir
}


# this test load a model, optimize it and check the prediction result of both cxx and light APIS.
function test_arm_predict_apis {
    local port=$1
    local workspace=$2
    local naive_model_path=$3
    local api_test_path=$(find . -name "test_apis")
    # the model is pushed to ./lite_naive_model
    adb -s emulator-${port} push ${naive_model_path} ${workspace}
    adb -s emulator-${port} push $api_test_path ${workspace}

    # test cxx_api first to store the optimized model.
    adb -s emulator-${port} shell ./test_apis --model_dir ./lite_naive_model --optimized_model ./lite_naive_model_opt
}


# Build the code and run lite arm tests. This is executed in the CI system.
function build_test_arm {
    ########################################################################
    # job 1-4 must be in one runner
    port_armv8=5554
    port_armv7=5556

    build_test_arm_subtask_android
    build_test_arm_subtask_armlinux
}

function build_test_npu {
    local test_name=$1
    local port_armv8=5554
    local port_armv7=5556
    local os=android
    local abi=armv8
    local lang=gcc

    local test_model_name=test_mobilenetv1 
    local model_name=mobilenet_v1
    cur_dir=$(pwd)

    build_npu "android" "armv8" "gcc" $test_name

    # just test the model on armv8
    # prepare_emulator $port_armv8

    if [[ "${test_name}x" != "x" ]]; then
        test_npu ${test_name} ${port_armv8}
    else
        # run_gen_code_test ${port_armv8}
        for _test in $(cat $TESTS_FILE | grep npu); do
            test_npu $_test $port_armv8
        done
    fi

    test_npu_model $test_model_name $port_armv8 "./third_party/install/$model_name"
    cd -
    # just test the model on armv8
    # adb devices | grep emulator | cut -f1 | while read line; do adb -s $line emu kill; done
    echo "Done"
}

function mobile_publish {
    # only check os=android abi=armv8 lang=gcc now
    local os=android
    local abi=armv8
    local lang=gcc

    # Install java sdk tmp, remove this when Dockerfile.mobile update
    apt-get install -y --no-install-recommends default-jdk

    cur_dir=$(pwd)
    build_dir=$cur_dir/build.lite.${os}.${abi}.${lang}
    mkdir -p $build_dir
    cd $build_dir

    cmake .. \
        -DWITH_GPU=OFF \
        -DWITH_MKL=OFF \
        -DWITH_LITE=ON \
        -DLITE_WITH_CUDA=OFF \
        -DLITE_WITH_X86=OFF \
        -DLITE_WITH_ARM=ON \
        -DLITE_WITH_LIGHT_WEIGHT_FRAMEWORK=ON \
        -DWITH_TESTING=OFF \
        -DLITE_WITH_JAVA=ON \
        -DLITE_SHUTDOWN_LOG=ON \
        -DLITE_ON_TINY_PUBLISH=ON \
        -DARM_TARGET_OS=${os} -DARM_TARGET_ARCH_ABI=${abi} -DARM_TARGET_LANG=${lang}

    make publish_inference -j$NUM_CORES_FOR_COMPILE
    cd - > /dev/null
}

############################# MAIN #################################
function print_usage {
    echo -e "\nUSAGE:"
    echo
    echo "----------------------------------------"
    echo -e "cmake_x86: run cmake with X86 mode"
    echo -e "cmake_cuda: run cmake with CUDA mode"
    echo -e "--arm_os=<os> --arm_abi=<abi> cmake_arm: run cmake with ARM mode"
    echo
    echo -e "build: compile the tests"
    echo -e "--test_name=<test_name> build_single: compile single test"
    echo
    echo -e "test_server: run server tests"
    echo -e "--test_name=<test_name> --adb_port_number=<adb_port_number> test_arm_android: run arm test"
    echo "----------------------------------------"
    echo
}

function main {
    # Parse command line.
    for i in "$@"; do
        case $i in
            --tests=*)
                TESTS_FILE="${i#*=}"
                shift
                ;;
            --test_name=*)
                TEST_NAME="${i#*=}"
                shift
                ;;
            --arm_os=*)
                ARM_OS="${i#*=}"
                shift
                ;;
            --arm_abi=*)
                ARM_ABI="${i#*=}"
                shift
                ;;
            --arm_lang=*)
                ARM_LANG="${i#*=}"
                shift
                ;;
            --arm_port=*)
                ARM_PORT="${i#*=}"
                shift
                ;;
            build)
                build $TESTS_FILE
                build $LIBS_FILE
                shift
                ;;
            build_single)
                build_single $TEST_NAME
                shift
                ;;
            cmake_x86)
                cmake_x86
                shift
                ;;
            cmake_opencl)
                cmake_opencl $ARM_OS $ARM_ABI $ARM_LANG
                shift
                ;;
            cmake_cuda)
                cmake_cuda
                shift
                ;;
            cmake_arm)
                cmake_arm $ARM_OS $ARM_ABI $ARM_LANG
                shift
                ;;
            build_opencl)
                build_opencl $ARM_OS $ARM_ABI $ARM_LANG
                shift
                ;;
            build_arm)
                build_arm $ARM_OS $ARM_ABI $ARM_LANG
                shift
                ;;
            test_server)
                test_server
                shift
                ;;
            test_arm)
                test_arm $ARM_OS $ARM_ABI $ARM_LANG $ARM_PORT
                shift
                ;;
            test_npu)
                test_npu $TEST_NAME $ARM_PORT
                shift
                ;;
            test_arm_android)
                test_arm_android $TEST_NAME $ARM_PORT
                shift
                ;;
            build_test_server)
                build_test_server
                shift
                ;;
            build_test_train)
                build_test_train
                shift
                ;;
            build_test_arm)
                build_test_arm
                shift
                ;;
            build_test_npu)
                build_test_npu $TEST_NAME
                shift
                ;;
            build_test_arm_opencl)
                build_test_arm_opencl
                shift
                ;;
            build_test_arm_subtask_android)
                build_test_arm_subtask_android
                shift
                ;;
            build_test_arm_subtask_armlinux)
                build_test_arm_subtask_armlinux
                shift
                ;;
            build_test_arm_model_mobilenetv1)
                build_test_arm_subtask_model test_mobilenetv1 mobilenet_v1
                build_test_arm_subtask_model test_mobilenetv1_int8 MobileNetV1_quant
                shift
                ;;
            build_test_arm_model_mobilenetv2)
                build_test_arm_subtask_model test_mobilenetv2 mobilenet_v2_relu
                shift
                ;;
            build_test_arm_model_resnet50)
                build_test_arm_subtask_model test_resnet50 resnet50
                shift
                ;;
            build_test_arm_model_inceptionv4)
                build_test_arm_subtask_model test_inceptionv4 inception_v4_simple
                shift
                ;;
            check_style)
                check_style
                shift
                ;;
            check_need_ci)
                check_need_ci
                shift
                ;;
            mobile_publish)
                mobile_publish
                shift
                ;;
            *)
                # unknown option
                print_usage
                exit 1
                ;;
        esac
    done
}

main $@
