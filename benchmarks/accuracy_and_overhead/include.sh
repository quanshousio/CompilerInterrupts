#!/bin/bash

source $(dirname "${BASH_SOURCE[0]}")/../../src/env_var.sh

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

if [ -z ${DIR+x} ] || [ -z ${CUR_PATH+x} ]; then
  printf "${RED}This file expects variable DIR & CUR_PATH to be set before inclusion.\n${NC}"
  exit
fi

OUTLIER_THRESHOLD="${OUTLIER_THRESHOLD:-"20"}"

BUILD_LOG="${BUILD_LOG:-"$DIR/build_log.txt"}"
ERROR_LOG="${ERROR_LOG:-"$DIR/error_log.txt"}"
CMD_LOG="${CMD_LOG:-"$DIR/cmd_log.txt"}"
OUTLIER_LOG="${OUTLIER_LOG:-"$DIR/outlier.txt"}"
OUT_FILE="${OUT_FILE:-"$DIR/out"}"
LIBCALL_WRAPPER_PATH="$CUR_PATH/libcall_wrapper.so"

PHOENIX_INPUT_PATH="$AO_INPUT_DIRECTORY/phoenix/"
PARSEC_INPUT_PATH="$AO_INPUT_DIRECTORY/parsec/"
SPLASH2_INPUT_PATH="$AO_INPUT_DIRECTORY/splash2/"

OPT_TL=2
NAIVE_TL=4
CD_TL=6
LEGACY_ACC=8
OPT_ACC=9
LEGACY_TL=10
NAIVE_ACC=11
OPT_INTERMEDIATE=12
NAIVE_INTERMEDIATE=13
OPT_CYCLES=17
ALL_INST_TL=19
ALL_INST_CYCLES=20

# Run types
PTHREAD_RUN=0
CI_RUN=1
HW_PC_RUN=2

splash2_benches="water-nsquared water-spatial ocean-cp ocean-ncp barnes volrend fmm raytrace radiosity radix fft lu-c lu-nc cholesky"
phoenix_benches="reverse_index histogram kmeans pca matrix_multiply string_match linear_regression word_count"
parsec_benches="blackscholes fluidanimate swaptions canneal streamcluster dedup"

mkdir -p $DIR
rm -f $BUILD_LOG $ERROR_LOG $CMD_LOG $OUTLIER_LOG $OUT_FILE

run_command() {
  command=$@
  echo $command | tee -a $CMD_LOG
  eval $command
  error_status=$?
  if [ $error_status -ne 0 ]; then
    printf "${RED}Command failed with status $error_status.\nFailed command: $command \n${NC}" | tee -a $CMD_LOG
    echo "Current path: "`pwd`
    return $error_status
  fi
}

get_median() {
  num_elem=0
  median=`echo $@ | \
  tr ' ' '\n' | \
  sort --parallel=32 -n | \
  awk 'BEGIN {OFMT="%f"} {lines[i++]=$1} 
  END { if(i>2) { for(l in lines){if(l/(i-1) >= 0.5) { print lines[l]; exit } } }
        else {print lines[0]; exit} }'`
  echo "Median over {$@} with $# elements: $median" >> $CMD_LOG
  echo $median
}

get_median_debug() {
  num_elem=0
  echo "Elements: $@"
  echo $@ | \
  tr ' ' '\n' | \
  sort --parallel=32 -n | \
  awk 'BEGIN {OFMT="%f"} {lines[i++]=$1} 
  END { if(i>2) { for(l in lines){if(l/(i-1) >= 0.5) { print lines[l]; exit } } }
        else {print lines[0]; exit} }'
}

get_avg() {
  num_elem=0
  for elem in $@; do
    num_elem=`expr $num_elem + 1`
    sum_str="$sum_str$elem + "
  done
  if [ $num_elem -ne 0 ]; then
    sum_str="${sum_str:0:${#sum_str}-2}"
    avg=`echo "scale=2;(($sum_str)/$num_elem)" | bc`
    echo "Average over $sum_str with $num_elem elements: $avg" >> $CMD_LOG
  else
    echo "No elements present to compute average!!" >> $CMD_LOG
    avg=0
  fi
  echo $avg
}

# $1 is measure, $2 is element to compare
is_outlier() {
  result=`echo "$1 $2" | awk -v thresh=$OUTLIER_THRESHOLD '
  {
    if($1 > $2) {
      if (($1-$2) > ((thresh*$1)/100))
        print $2
    } else {
      if (($2-$1) > ((thresh*$1)/100))
        print $2
    }
  } 
  '`
  echo $result
}

# outliers are printed in outlier log
# pass in a string in variable "header" to describe the data
get_outliers() {
  measure=$1
  outliers=""
  i=0
  j=0
  for elem in ${@:2}; do
    res=$(is_outlier $measure $elem)
    if [ ! -z "$res" ]; then
      outliers=$outliers$res" "
      j=`expr $j + 1`
    fi
    i=`expr $i + 1`
  done
  if [ $j -ne 0 ]; then
    pc_outlier=`echo "$j*100/$i" | bc`
    echo -e "$header\
      \n#elements: $i, #outliers: $j (${pc_outlier}%), measure: $measure\
      \noutliers: $outliers\
      \nelements: ${@:2}" >> $OUTLIER_LOG
  fi
}

# format: $1 is the measure used, $2 is the interval corresponding to main thread, that is, the first element of the set of medians
# assumption: main thread has lowest thread id
is_main_thread_outlier() {
  measure=$1
  main_thread_intv=$2
  res=$(is_outlier $measure $main_thread_intv)
  if [ ! -z "$res" ]; then
    echo -e "$header Main thread interval ($main_thread_intv) is an outlier w.r.t $measure" >> $OUTLIER_LOG
  fi
  echo "" >> $OUTLIER_LOG
}

# format: $1 is the measure used, last element is the interval corresponding to the last thread
is_last_thread_outlier() {
  measure=$1
  last_thread_intv=${@: -1}
  res=$(is_outlier $measure $last_thread_intv)
  if [ ! -z "$res" ]; then
    echo -e "$header Last thread interval ($last_thread_intv) is an outlier w.r.t $measure" >> $OUTLIER_LOG
  fi
  echo "" >> $OUTLIER_LOG
}

get_allowed_dev_setting() {
  if [ $1 -eq 8 ] || [ $1 -eq 9 ] || [ $1 -eq 11 ]; then
    allowed_dev=0
  elif [ $1 -eq 10 ]; then
    allowed_dev=1
  else
    allowed_dev=100
  fi
  echo $allowed_dev
}

set_benchmark_info() {
  target_app="$1"

  ROOT_DIR="./"
  for app in $splash2_benches; do
    SUITE_DIR="${ROOT_DIR}/splash2/codes/"
    if [ "$target_app" == "$app" ]; then
      BENCH_SUITE="splash2"
      case "$target_app" in
      "radix" | "fft" | "cholesky")
        BUILD_DIR="${SUITE_DIR}/kernels"
        BENCH_DIR="${SUITE_DIR}/kernels/$target_app"
        ;;
      "lu-c")
        BUILD_DIR="${SUITE_DIR}/kernels"
        BENCH_DIR="${SUITE_DIR}/kernels/lu/contiguous_blocks"
        ;;
      "lu-nc")
        BUILD_DIR="${SUITE_DIR}/kernels"
        BENCH_DIR="${SUITE_DIR}/kernels/lu/non_contiguous_blocks"
        ;;
      "ocean-cp")
        BUILD_DIR="${SUITE_DIR}/apps"
        BENCH_DIR="${SUITE_DIR}/apps/ocean/contiguous_partitions"
        ;;
      "ocean-ncp")
        BUILD_DIR="${SUITE_DIR}/apps"
        BENCH_DIR="${SUITE_DIR}/apps/ocean/non_contiguous_partitions"
        ;;
      *)
        BUILD_DIR="${SUITE_DIR}/apps"
        BENCH_DIR="${SUITE_DIR}/apps/$target_app"
        ;;
      esac
      return
    fi
  done

  for app in $phoenix_benches; do
    SUITE_DIR="${ROOT_DIR}/phoenix/phoenix-2.0/"
    if [ "$target_app" == "$app" ]; then
      BENCH_SUITE="phoenix"
      BUILD_DIR="${SUITE_DIR}/"
      # BENCH_DIR="${SUITE_DIR}/tests/$target_app"
      BENCH_DIR="${SUITE_DIR}/"
      return
    fi
  done

  for app in $parsec_benches; do
    SUITE_DIR="${ROOT_DIR}/parsec-benchmark/pkgs/"
    if [ "$target_app" == "$app" ]; then
      BENCH_SUITE="parsec"
      case "$target_app" in
      "canneal" | "dedup" | "streamcluster")
        BUILD_DIR="${SUITE_DIR}/kernels"
        BENCH_DIR="${SUITE_DIR}/kernels/$app/src/"
        ;;
      *)
        BUILD_DIR="${SUITE_DIR}/apps"
        BENCH_DIR="${SUITE_DIR}/apps/$app/src/"
        ;;
      esac
      return
    fi
  done
  BUILD_DIR="unavailable"
  BENCH_DIR="unavailable"
}

get_ci_str() {
  case "$1" in
    2) ci_type="CI";;
    4) ci_type="Naive";;
    6) ci_type="Coredet";;
    8) ci_type="legacy-acc";;
    9) ci_type="opt-acc";;
    10) ci_type="CnB";;
    11) ci_type="naive-acc";;
    12) ci_type="CI-cycles";;
    13) ci_type="Naive-cycles";;
    19) ci_type="All-inst";;
    20) ci_type="All-inst-cycles";;
    *)
      echo "Wrong CI Type $1"
      exit
    ;;
  esac
  echo $ci_type
}

get_ci_str_in_lower_case() {
  ci_str=$(get_ci_str $1)
  echo "$ci_str" | tr '[:upper:]' '[:lower:]'
}

read_tune_param() {
  case "$2" in
    2) ci_type="CI";;
    4) ci_type="Naive";;
    6) ci_type="Coredet";;
    8) ci_type="legacy-acc";;
    9) ci_type="opt-acc";;
    10) ci_type="CnB";;
    11) ci_type="naive-acc";;
    12) ci_type="CI-cycles";;
    13) ci_type="Naive-cycles";;
    19) ci_type="All-inst";;
    20) ci_type="All-inst-cycles";;
    *)
      echo "Wrong CI Type $1"
      exit
    ;;
  esac
  if [ $2 -eq 8 ]; then
    intv=5000
  else
    tune_file="${CUR_PATH}/predicted-${ci_type}-th$3-${CYCLE}.txt"
    #tune_file="${CUR_PATH}/${ci_type}-tuning-th$3-${CYCLE}.txt"
    #tune_file="${CUR_PATH}/Naive-tuning-th$3-${CYCLE}.txt"
    while read line; do
      present=`echo $line | grep $1 | wc -l`
      if [ $present -eq 1 ]; then
        intv=`echo $line | cut -d' ' -f 2`
        break
      fi
    done < $tune_file
  fi
  echo $intv
}

get_executable_name() {
  program=$1
  suffix_conf=$2
  declare suffix
  set_benchmark_info $program
  if [ "$BENCH_SUITE" == "splash2" ]; then
    if [ $suffix_conf -ne $CI_RUN ]; then
      suffix="-orig"
    else
      suffix="-lc"
    fi
  elif [ "$BENCH_SUITE" == "parsec" ]; then
    if [ $suffix_conf -ne $CI_RUN ]; then
      suffix="_llvm"
    else
      suffix="_ci"
    fi
  fi
  echo "${program}${suffix}"
}

kill_all_processes() {
  all_apps="$splash2_benches $phoenix_benches $parsec_benches"

  proc_running=0
  for app in $all_apps; do
    proc_present=`ps -aef | grep $app | grep -v grep`
    if [ ! -z "$proc_present" ]; then
      printf "${RED}ATTENTION: Process is running!! ($proc_present) Will try to kill them.${NC}\n" | tee -a $CMD_LOG
      proc_running=1
      break
    fi
    if [ $proc_running -eq 0 ]; then
      #echo "No proc running"
      return
    fi
  done

  for app in $all_apps; do
    ci_exec_name=$(get_executable_name $app $CI_RUN)
    orig_exec_name=$(get_executable_name $app $PTHREAD_RUN)
    #echo "Killing $ci_exec_name $orig_exec_name"
    sudo pkill $ci_exec_name 
    sudo pkill $orig_exec_name
    proc_present=`ps -aef | grep $app | grep -v grep`
    while [ ! -z "$proc_present" ]; do
      printf "${RED}ATTENTION: Process ($app) is still running!! ($proc_present) Trying to kill again.${NC}\n" | tee -a $CMD_LOG
    done
  done

  sleep 2
}

is_a_long_duration_app() {
  case "$1" in
    "swaptions") echo "1";;
    "lu-c") echo "1";;
    "radix") echo "1";;
    "canneal") echo "1";;
    *) echo 0;;
  esac
}

# $1-program name
# $2-no. of threads
# $3-0 if orig program is run, 1 if CI-based program is run
# $4-target interval in IR (mandatory - set to 0 when not used)
# $5-target interval in cycles (mandatory - set to 0 when not used)
# PREFIX variable (opt) to have any prefix for the command to be run
get_program_cmd() {

  program=$1
  th=$2
  suffix_conf=$3

  if [ $# -ne 5 ];then
    echo "Usage: get_program_cmd <bench> <thread> <run type> <0 or IR interval> <0 or cycle interval>"
    exit
  fi 

  rm -f $OUT_FILE
  executable_name=$(get_executable_name $program $suffix_conf)

  unset prefix

  if [ $4 -ne 0 ];then
    prefix=$prefix"CI_IR_INTERVAL=$4 "
  fi

  # for safety purposes, there is an assert in code if this variable is not set for any CI run
  prefix=$prefix"CI_CYCLES_INTERVAL=$5 "
  prefix=$prefix"$PREFIX timeout 2m "

  case "$program" in
    water-nsquared)
      command="$prefix ./$executable_name < ${SPLASH2_INPUT_PATH}/$program/input.$th > $OUT_FILE; sleep 0.5"
    ;;
    water-spatial)
      command="$prefix ./$executable_name < ${SPLASH2_INPUT_PATH}/$program/input.$th > $OUT_FILE; sleep 0.5"
    ;;
    ocean-cp) 
      command="$prefix ./$executable_name -n1026 -p $th -e1e-07 -r2000 -t28800 > $OUT_FILE"
    ;;
    ocean-ncp) 
      command="$prefix ./$executable_name -n258 -p $th -e1e-07 -r2000 -t28800 > $OUT_FILE"
    ;;
    barnes)
      command="$prefix ./$executable_name < ${SPLASH2_INPUT_PATH}/$program/input.$th > $OUT_FILE"
    ;;
    volrend)
      command="$prefix ./$executable_name $th ${SPLASH2_INPUT_PATH}/$program/inputs/head > $OUT_FILE"
    ;;
    fmm)
      command="$prefix ./$executable_name < ${SPLASH2_INPUT_PATH}/$program/inputs/input.65535.$th > $OUT_FILE"
    ;;
    raytrace)
      command="$prefix ./$executable_name -p $th -m72 ${SPLASH2_INPUT_PATH}/$program/inputs/balls4.env > $OUT_FILE"
    ;;
    radiosity)
      command="$prefix ./$executable_name -p $th -batch -largeroom > $OUT_FILE"
    ;;
    radix)
      command="$prefix ./$executable_name -p$th -n134217728 -r1024 -m524288 > $OUT_FILE"
    ;;
    fft)
      command="$prefix ./$executable_name -m24 -p$th -n1048576 -l4 > $OUT_FILE"
    ;;
    lu-c)
      command="$prefix ./$executable_name -n4096 -p$th -b16 > $OUT_FILE"
    ;;
    lu-nc)
      command="$prefix ./$executable_name -n2048 -p$th -b16 > $OUT_FILE"
    ;;
    cholesky)
      command="$prefix ./$executable_name -p$th -B32 -C1024 ${SPLASH2_INPUT_PATH}/$program/inputs/tk29.O > $OUT_FILE"
    ;;
    histogram)
      command="MR_NUMTHREADS=$th $prefix ./tests/$program/$executable_name ${PHOENIX_INPUT_PATH}/input_datasets/${program}_datafiles/large.bmp > $OUT_FILE 2>&1"
    ;;
    kmeans)
      command="MR_NUMTHREADS=$th $prefix ./tests/$program/$executable_name -d 100 -c 10 -p 500000 -s 50 > $OUT_FILE 2>&1"
    ;;
    pca) 
      command="MR_NUMTHREADS=$th $prefix ./tests/$program/$executable_name -r 1000 -c 1000 -s 1000 > $OUT_FILE 2>&1"
    ;;
    matrix_multiply) 
      command="MR_NUMTHREADS=$th $prefix ./tests/$program/$executable_name 900 600 1 > $OUT_FILE 2>&1"
    ;;
    string_match)
      command="MR_NUMTHREADS=$th $prefix ./tests/$program/$executable_name ${PHOENIX_INPUT_PATH}/input_datasets/${program}_datafiles/key_file_100MB.txt > $OUT_FILE 2>&1"
    ;;
    linear_regression)
      command="MR_NUMTHREADS=$th $prefix ./tests/$program/$executable_name ${PHOENIX_INPUT_PATH}/input_datasets/${program}_datafiles/key_file_500MB.txt > $OUT_FILE 2>&1"
    ;;
    word_count)
      command="MR_NUMTHREADS=$th $prefix ./tests/$program/$executable_name ${PHOENIX_INPUT_PATH}/input_datasets/${program}_datafiles/word_50MB.txt > $OUT_FILE 2>&1"
    ;;
    reverse_index)
      command="MR_NUMTHREADS=$th $prefix ./tests/$program/$executable_name ${PHOENIX_INPUT_PATH}/input_datasets/${program}_datafiles/www.stanford.edu/dept/ > $OUT_FILE 2>&1"
    ;;
    blackscholes)
      command="$prefix ./$executable_name $th ${PARSEC_INPUT_PATH}/$program/inputs/in_64K.txt prices.txt > $OUT_FILE; sleep 0.5"
    ;;
    fluidanimate)
      command="$prefix ./$executable_name $th 5 ${PARSEC_INPUT_PATH}/$program/inputs/in_300K.fluid out.fluid > $OUT_FILE; sleep 0.5"
    ;;
    swaptions)
      command="$prefix ./$executable_name -ns 128 -sm 100000 -nt $th > $OUT_FILE 2>&1; sleep 0.5" 
    ;;
    canneal)
      command="$prefix ./$executable_name $th 15000 2000 ${PARSEC_INPUT_PATH}/$program/inputs/200000.nets 6000 > $OUT_FILE; sleep 0.5"
    ;;
    dedup)
      command="$prefix ./$executable_name -c -p -v -t $th -i ${PARSEC_INPUT_PATH}/$program/inputs/media.dat -o output.dat.ddp -w none > $OUT_FILE; sleep 0.5" 
    ;;
    streamcluster)
      command="$prefix ./$executable_name 10 20 128 16384 16384 1000 none output.txt $th > $OUT_FILE; sleep 0.5" 
    ;;
  esac
  echo -e "Command for $program running $th threads:-\n$command" >> $CMD_LOG
  echo $command
}

get_binary_size() {
  if [ $# -ne 2 ]; then
    printf "${RED}get_binary_size requires 2 arguments.\n${NC}"
    exit
  fi

  bench=$1
  runtype=$2

  set_benchmark_info $bench
  pushd $BENCH_DIR > /dev/null

  executable_name=$(get_executable_name $bench $runtype)
  if [ "$BENCH_SUITE" == "phoenix" ]; then
    sz=`ls -l ./tests/$bench/$executable_name | awk '{print $5}'`
  else
    sz=`ls -l $executable_name | awk '{print $5}'`
  fi

  popd > /dev/null

  echo $sz
}

# $1-program name, $2 - 0:orig run, 1:ci run
#program output will be written in $OUT_FILE
#PREFIX variable (opt) to contain any required prefix to the command
dry_run_exp() {
  if [ $# -ne 2 ]; then
    printf "${RED}dry_run_exp requires 2 arguments.\n${NC}"
    exit
  fi

  bench=$1
  runtype=$2

  printf "${GREEN}Dry run:-\n${NC}" | tee -a $CMD_LOG

  set_benchmark_info $bench
  pushd $BENCH_DIR > /dev/null

  command=$(get_program_cmd $bench 1 $runtype 100000 100000) # the hardcoded parameters do not matter
  run_command ${command}

  popd > /dev/null
}

#$1: program name, $2: run type - pthread, ci, hw perf counter, $3:#thread, $4:ci setting if run type is ci, $5 - target ir (read from file, if 0), $6 - target cycle
#program output will be written in $OUT_FILE
#PREFIX variable (opt) to contain any required prefix to the command
run_exp() {
  if [ $# -ne 6 ]; then
    printf "${RED}run_exp requires 6 arguments.\nCurrent arguments: $@\n${NC}"
    exit
  fi

  local bench=$1
  local runtype=$2
  local thread=$3
  local ci_setting=$4
  local intv_ir=$5
  local intv_cycle=$6

  kill_all_processes

  if [ $runtype -eq $CI_RUN ]; then
    # Sanity checks
    if [ $ci_setting -eq 0 ]; then
      echo "run_exp(): CI Type cannot be 0 for a CI run. Aborting."; exit
    fi
    if [ $ci_setting -eq $OPT_INTERMEDIATE ] && [ $intv_cycle -eq 0 ]; then
      echo "run_exp(): Target interval in cycles cannot be 0 for a CI-cycles run. Aborting."; exit
    fi
    if [ $intv_ir -eq 0 ]; then
      # Read from file
      echo "Reading target interval in IR from file" >> $CMD_LOG
      intv_ir=$(read_tune_param $bench $ci_setting $thread)
    fi
  elif [ $runtype -eq $PTHREAD_RUN ]; then
    intv_ir=0
    intv_cycle=0
  elif [ $runtype -eq $HW_PC_RUN ]; then
    if [ $intv_cycle -eq 0 ]; then
      echo "run_exp(): Target interval in cycles cannot be 0 for a HW performance counter based run. Aborting."; exit
    fi
  else
    echo "run_exp(): Run type $run_type is not valid. Aborting."; exit
  fi


  printf "${GREEN}Experiment run (Target Cycles: $intv_cycle, Target IR: $intv_ir):-\n${NC}" | tee -a $CMD_LOG

  set_benchmark_info $bench
  pushd $BENCH_DIR > /dev/null

  command=$(get_program_cmd $bench $thread $runtype $intv_ir $intv_cycle)
  run_command ${command}

  popd > /dev/null
}

build_orig() {
  BENCH=$1
  THREAD=$2

  set_benchmark_info $BENCH

  pushd $BUILD_DIR > /dev/null

  echo -e "\nBuilding $BENCH for PThread for $THREAD thread(s): " | tee -a $CMD_LOG $BUILD_LOG

  BUILD_PREFIX="EXTRA_FLAGS=\"$EXTRA_FLAGS\""
  if [ "$BENCH_SUITE" == "splash2" ]; then
    cmd="BUILD_LOG=$BUILD_LOG ERROR_LOG=$ERROR_LOG make -f Makefile.orig $BENCH-clean;"
    cmd=$cmd"BUILD_LOG=$BUILD_LOG ERROR_LOG=$ERROR_LOG $BUILD_PREFIX make -f Makefile.orig $BENCH"
  elif [ "$BENCH_SUITE" == "phoenix" ]; then
    cmd="$BUILD_PREFIX make -f Makefile.orig clean >$BUILD_LOG 2>$ERROR_LOG;"
    cmd=$cmd"make -f Makefile.orig >$BUILD_LOG 2>$ERROR_LOG"
  elif [ "$BENCH_SUITE" == "parsec" ]; then
    cmd="BUILD_LOG=$BUILD_LOG ERROR_LOG=$ERROR_LOG make -f Makefile.llvm clean;"
    cmd=$cmd"BUILD_LOG=$BUILD_LOG ERROR_LOG=$ERROR_LOG $BUILD_PREFIX make -f Makefile.llvm $BENCH"
  fi
  run_command $cmd

  popd > /dev/null
}

build_ci() {
  BENCH=$1
  CI_SETTING=$2
  THREAD=$3

  set_benchmark_info $BENCH

  pushd $BUILD_DIR > /dev/null

  if [ $# -eq 3 ]; then
    PI=$(read_tune_param $BENCH $CI_SETTING $THREAD)
    #CI=`echo "scale=0; $PI/5" | bc`
    #CI=`echo "scale=0; $CYCLE/5" | bc`
  else
    PI=$4
  fi

  if [ $PI -ge 5000 ]; then
    CI=1000
  elif [ $PI -ge 1000 ]; then
    CI=500
  else
    CI=100
  fi

  AD=$(get_allowed_dev_setting $CI_SETTING)

  BUILD_PREFIX="ALLOWED_DEVIATION=$AD CLOCK_TYPE=1 PUSH_INTV=$PI CMMT_INTV=$CI CYCLE_INTV=$CYCLE INST_LEVEL=$CI_SETTING EXTRA_FLAGS=\"$EXTRA_FLAGS\""
  echo -e "\nBuilding $BENCH for $THREAD thread(s) with $BUILD_PREFIX: " | tee -a $CMD_LOG $BUILD_LOG

  if [ "$BENCH_SUITE" == "splash2" ]; then
    cmd="BUILD_LOG=$BUILD_LOG ERROR_LOG=$ERROR_LOG make -f Makefile.lc $BENCH-clean;"
    cmd=$cmd"BUILD_LOG=$BUILD_LOG ERROR_LOG=$ERROR_LOG $BUILD_PREFIX make -f Makefile.lc $BENCH"
  elif [ "$BENCH_SUITE" == "phoenix" ]; then
    cmd="make -f Makefile.lc clean >$BUILD_LOG 2>$ERROR_LOG;"
    cmd=$cmd"$BUILD_PREFIX make -f Makefile.lc >$BUILD_LOG 2>$ERROR_LOG"
  elif [ "$BENCH_SUITE" == "parsec" ]; then
    cmd="BUILD_LOG=$BUILD_LOG ERROR_LOG=$ERROR_LOG make -f Makefile.ci clean;"
    cmd=$cmd"BUILD_LOG=$BUILD_LOG ERROR_LOG=$ERROR_LOG $BUILD_PREFIX make -f Makefile.ci $BENCH"
  fi
  run_command $cmd

  cat $BUILD_LOG $ERROR_LOG > $DIR/log-$BENCH-ci$CI_SETTING-th$THREAD.txt

  popd > /dev/null
}

# Create CDF based on column 4
# $1 - input file, $2 - output cdf file, $3 (opt) - output sampling file
create_cdf() {
  ifile=$1
  cdf_file=$2
  ofile=$cdf_file

  # filter out lines with alphabets & create CDF from column 4
  cat $ifile |\
  awk '/[0-9]+/ && !/[a-zA-Z]+/ {print $4}' |\
  sort --parallel=32 -n \
  > $cdf_file 
  printf "${GREEN}Created $cdf_file in CDF format by processing $ifile\n${NC}" | tee -a $CMD_LOG

  #awk 'BEGIN {OFMT="%f"} {lines[i++]=$0} END {for(l in lines){print (i>1) ? (l/(i-1)) : i," ",lines[l]}}' \
  #sort --parallel=32 -n -k 2 \

  # sample
  if [ $# -ge 3 ]; then

    sampling_file=$3
    ofile=$sampling_file

    gawk -v nlines="$(cat $cdf_file | wc -l)" 'nlines<1000 || NR % int(nlines/100) == 1 {print} {line=$0} END {print line}' $cdf_file |\
    awk 'BEGIN {OFMT="%f"} {lines[i++]=$0} END {for(l in lines){print (i>1) ? (l/(i-1)) : i," ",lines[l]}}' > $sampling_file
    #rm -f $cdf_file
    printf "${GREEN}Sampled $cdf_file to $sampling_file, 1 for every 100 data points (> 1000 data point files)\n${NC}" | tee -a $CMD_LOG

    # Print percentile statistics
    printf "${GREEN}$ifile statistics (with $(wc -l $ifile | awk '{print $1}') data points):-\n${NC}" | tee -a $STAT_FILE
    awk '/^0.05/ {printf("5pc\t%s%d\n", ($2-5000)>0?"+":"", $2-5000); exit}' $ofile | tee -a $STAT_FILE
    awk '/^0.1/ {printf("10pc\t%s%d\n", ($2-5000)>0?"+":"", $2-5000); exit}' $ofile | tee -a $STAT_FILE
    awk '/^0.25/ {printf("25pc\t%s%d\n", ($2-5000)>0?"+":"", $2-5000); exit}' $ofile | tee -a $STAT_FILE
    awk '/^0.5/ {printf("50pc\t%s%d\n", ($2-5000)>0?"+":"", $2-5000); exit}' $ofile | tee -a $STAT_FILE
    awk '/^0.75/ {printf("75pc\t%s%d\n", ($2-5000)>0?"+":"", $2-5000); exit}' $ofile | tee -a $STAT_FILE
    awk '/^0.9/ {printf("90pc\t%s%d\n", ($2-5000)>0?"+":"", $2-5000); exit}' $ofile | tee -a $STAT_FILE
    awk '/^0.95/ {printf("95pc\t%s%d\n", ($2-5000)>0?"+":"", $2-5000); exit}' $ofile | tee -a $STAT_FILE

  else

    printf "${GREEN}$ifile statistics (with $(wc -l $ifile | awk '{print $1}') data points):-\n${NC}" | tee -a $STAT_FILE
    awk -v nlines="$(wc -l $ofile)" ' \
    FNR == nlines*(5/100) {printf("5pc\t%s%d\n", ($1-5000)>0?"+":"", $1-5000)};
    FNR == nlines*(10/100) {printf("10pc\t%s%d\n", ($1-5000)>0?"+":"", $1-5000)};
    FNR == nlines*(25/100) {printf("25pc\t%s%d\n", ($1-5000)>0?"+":"", $1-5000)};
    FNR == nlines*(50/100) {printf("50pc\t%s%d\n", ($1-5000)>0?"+":"", $1-5000)};
    FNR == nlines*(75/100) {printf("75pc\t%s%d\n", ($1-5000)>0?"+":"", $1-5000)};
    FNR == nlines*(90/100) {printf("90pc\t%s%d\n", ($1-5000)>0?"+":"", $1-5000)};
    FNR == nlines*(95/100) {printf("95pc\t%s%d\n", ($1-5000)>0?"+":"", $1-5000)};
    ' $ofile | tee -a $STAT_FILE

  fi
}

build_libcall_wrapper() {
  gcc -fPIC -shared $CUR_PATH/libcall_wrapper.c -I$CUR_PATH/../../src/ -o $LIBCALL_WRAPPER_PATH -ldl
}

print_end_notice() {
  printf "${GREEN}
    Check $CMD_LOG for errors (Search with \"Command failed\").
    Check $OUTLIER_LOG for outliers (Search for \"#outliers\") or \"No CI called\" or \"Main thread interval\" or \"Last thread interval\" or \"Total failed runs\".\n
    Any build logs & error logs can be found in $BUILD_LOG & $ERROR_LOG respectively.\n${NC}"
  if [ -f $OUTLIER_LOG ]; then
    print "Printing outlier log from $OUTLIER_LOG"
    cat $OUTLIER_LOG
  fi

}

quit_if_not_superuser() {
  if [ "$EUID" -ne 0 ]; then
    echo "Please run as root"
    exit
  fi
}
