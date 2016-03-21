#!/bin/bash

THIS_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
TMP_DIR=$THIS_DIR/tmp
SHADER_DIRS=$(ls -C1 $THIS_DIR/shaders)
REF_DIR=$THIS_DIR/reference
REPORT_FILE=$THIS_DIR/report.txt

# Compile to SPIR-V
for dir in ${SHADER_DIRS[*]}
do
    src_dir=$THIS_DIR/shaders/$dir
    src_shaders=$(ls -C1 $src_dir/*)

    spirv_dir=$TMP_DIR/spirv/$dir
    mkdir -p $spirv_dir

    for src_file in ${src_shaders[*]}
    do
        spirv_file=$spirv_dir/`basename $src_file`.spirv
        glslangvalidator -V -o $spirv_file $src_file
    done
done

# Back compile to GLSL
for dir in ${SHADER_DIRS[*]}
do
    src_dir=$THIS_DIR/shaders/$dir
    src_shaders=$(ls -C1 $src_dir/*)

    spirv_dir=$TMP_DIR/spirv/$dir

    out_dir=$TMP_DIR/shaders/$dir
    mkdir -p $out_dir

    for src_file in ${src_shaders[*]}
    do
        basename=`basename $src_file`;
        spirv_file=$spirv_dir/$basename.spirv
        glsl_file=$out_dir/$basename
        if [ -f "$spirv_file" ]; then
            echo "spir2cross --output $glsl_file $spirv_file"
            spir2cross --output $glsl_file $spirv_file
        fi
    done
done

echo ""

# Diff report
echo "This is a diff of the shader files that compiled succesffully to SPIR_V and back to GLSL." > $REPORT_FILE
echo "" >> $REPORT_FILE

for dir in ${SHADER_DIRS[*]}
do
    src_dir=$THIS_DIR/shaders/$dir
    src_shaders=$(ls -C1 $src_dir/*)

    spirv_dir=$TMP_DIR/spirv/$dir
    out_dir=$TMP_DIR/shaders/$dir
    ref_dir=$REF_DIR/shaders/$dir

    for src_file in ${src_shaders[*]}
    do
        basename=`basename $src_file`;
        spirv_file=$spirv_dir/$basename.spirv
        glsl_file=$out_dir/$basename
        ref_file=$ref_dir/$basename
        if [ -f "$glsl_file" ]; then
            echo "diff $ref_file $glsl_file"
            echo "SOURCE FILE   : $src_file" >> $REPORT_FILE
            echo "SPIR-V FILE   : $spirv_file" >> $REPORT_FILE
            echo "GLSL FILE     : $glsl_file" >> $REPORT_FILE
            echo "REF GLSL FILE : $ref_file" >> $REPORT_FILE
            echo "diff output:" >> $REPORT_FILE
            diff $ref_file $glsl_file >> $REPORT_FILE
            echo "end of diff output" >> $REPORT_FILE
            echo -e "\n" >> $REPORT_FILE
        fi
    done
done

echo -e "\nSee report.txt for more information!"
