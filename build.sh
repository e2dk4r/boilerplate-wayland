#!/bin/sh
# vi: set et ft=sh ts=2 sw=2 fenc=utf-8 :vi
export LC_ALL=C
export TZ=UTC

IsBuildDebug=1
IsBuildEnabled=1
IsTestsEnabled=1

PROJECT_NAME=test
OUTPUT_NAME=$PROJECT_NAME

usage() {
  cat <<EOF
  NAME
    build.sh [OPTIONS]
  
  DESCRIPTION
    Build script of $PROJECT_NAME.
  
  OPTIONS
    --debug
      Build with debugging information.

    --build-directory=path
      Build executables in this folder. If directory not exists, one will be
      created.

    --disable-$PROJECT_NAME
      Do not build $PROJECT_NAME binary.

    test
      Run tests.

    -h, --help
      Display help page.

  EXAMPLES
     $ ./build.sh
     Build only the $PROJECT_NAME

     $ ./build.sh test
     Run only the tests.
EOF
}

for i in "$@"; do
  case $i in
    --debug)
      IsBuildDebug=1
      ;;
    --build-directory=*)
      OutputDir="${i#*=}"
      ;;
    --disable-handmadehero)
      IsBuildEnabled=0
      ;;
    test|tests)
      IsBuildEnabled=0
      IsTestsEnabled=1
      ;;
    -h|-help|--help)
      usage
      exit 0
      ;;
    *)
      echo "argument $i not recognized"
      usage
      exit 1
      ;;
  esac
done

################################################################
# TEXT FUNCTIONS
################################################################

# [0,1] StringContains(string, search)
StringContains() {
  string="$1"
  search="$2"
  [ "${string##*$search}" != "$string" ] && echo 1 || echo 0
}

# [0,1] StringStartsWith(string, search)
StringStartsWith() {
  string="$1"
  search="$2"
  [ "${string#$search}" != "$string" ] && echo 1 || echo 0
}

# [0,1] StringEndsWith(string, search)
StringEndsWith() {
  string="$1"
  search="$2"
  [ "${string%$search}" != "$string" ] && echo 1 || echo 0
}

# string Basename(path)
Basename() {
  path="$1"
  echo "${path##*/}"
}

# string BasenameWithoutExtension(path)
BasenameWithoutExtension() {
  path="$1"
  basename="$(Basename "$path")"
  echo ${basename%.*}
}

# string Dirname(path)
Dirname() {
  path="$1"
  if [ $(StringStartsWith "$path" '/') ]; then
    dirname="${path%/*}"
    if [ -z "$dirname" ]; then
      echo '/'
    else
      echo $dirname
    fi
  else
    echo '.'
  fi
}

################################################################
# TIME FUNCTIONS
################################################################

StartTimer() {
  startedAt=$(date +%s)
}

StopTimer() {
  echo $(( $(date +%s) - $startedAt ))
}

################################################################
# LOG FUNCTIONS
################################################################

Timestamp="$(date +%Y%m%dT%H%M%S)"

Log() {
  string=$1
  output="$OutputDir/logs/build-$Timestamp.log"
  if [ ! -e "$(Dirname "$output")" ]; then
    mkdir "$(Dirname "$output")"
  fi
  echo "$string" >> "$output"
}

Debug() {
  string=$1
  output="$OutputDir/logs/build-$Timestamp.log"
  if [ ! -e "$(Dirname "$output")" ]; then
    mkdir "$(Dirname "$output")"
  fi
  echo "[DEBUG] $string" >> "$output"
}

################################################################

ProjectRoot="$(Dirname $(realpath "$0"))"
if [ "$(pwd)" != "$ProjectRoot" ]; then
  echo "Must be call from project root!"
  echo "  $ProjectRoot"
  exit 1
fi

OutputDir="${OutputDir:-$ProjectRoot/build}"
if [ ! -e "$OutputDir" ]; then
  mkdir "$OutputDir"

  # version control ignore
  echo '*' > "$OutputDir/.gitignore"

  echo 'syntax: glob' > "$OutputDir/.hgignore"
  echo '**/*' > "$OutputDir/.hgignore"
fi

IsOSLinux=$(StringEndsWith "$(uname)" 'Linux')

cc="${CC:-clang}"
IsCompilerGCC=$(StringStartsWith "$("$cc" --version | head -n 1 -c 32)" "gcc")
IsCompilerClang=$(StringStartsWith "$("$cc" --version | head -n 1 -c 32)" "clang")
if [ $IsCompilerGCC -eq 0 ] && [ $IsCompilerClang -eq 0 ]; then
  echo "unsupported compiler $cc. continue (y/n)?"
  read input
  if [ "$input" != 'y' ] && [ "$input" != 'Y' ]; then
    exit 1
  fi

  echo "Assuming $cc as GCC"
  IsCompilerGCC=1
fi

cflags="$CFLAGS"
# standard
cflags="$cflags -std=c99"
# performance
cflags="$cflags -O3"
if [ $(StringContains "$cflags" '-march=') -eq 0 ]; then
  cflags="$cflags -march=x86-64-v3"
fi
cflags="$cflags -funroll-loops"
cflags="$cflags -fomit-frame-pointer"
# warnings
cflags="$cflags -Wall -Werror"
cflags="$cflags -Wconversion"
cflags="$cflags -Wno-unused-parameter"
cflags="$cflags -Wno-unused-result"
cflags="$cflags -Wno-missing-braces"

cflags="$cflags -DCOMPILER_GCC=$IsCompilerGCC"
cflags="$cflags -DCOMPILER_CLANG=$IsCompilerClang"

cflags="$cflags -DIS_BUILD_DEBUG=$IsBuildDebug"
if [ $IsBuildDebug -eq 1 ]; then
  cflags="$cflags -g -O0"
  cflags="$cflags -Wno-unused-but-set-variable"
  cflags="$cflags -Wno-unused-function"
  cflags="$cflags -Wno-unused-variable"
fi

if [ $IsOSLinux -eq 1 ]; then
  # needed by c libraries
  cflags="$cflags -D_GNU_SOURCE=1"
  cflags="$cflags -D_XOPEN_SOURCE=700"
fi

ldflags="${LDFLAGS}"
ldflags="$ldflags -Wl,--as-needed"
ldflags="${ldflags# }"

Log "Started at $(date '+%Y-%m-%d %H:%M:%S')"
Log "================================================================"
Log "root:      $ProjectRoot"
Log "build:     $OutputDir"
Log "os:        $(uname)"
Log "compiler:  $cc"

Log "cflags: $cflags"
if [ ! -z "$CFLAGS" ]; then
  Log "from your env: $CFLAGS"
fi

Log "ldflags: $ldflags"
if [ ! -z "$LDFLAGS" ]; then
  Log "from your env: $LDFLAGS"
fi
Log "================================================================"

LIB_M='-lm'

if [ $IsBuildEnabled -eq 1 ]; then
  if [ $IsOSLinux -eq 0 ]; then
    echo "Do not know how to compile on this OS"
    echo "  OS: $(uname)"
    exit 1
  elif [ $IsOSLinux -eq 1 ]; then
    ################################################################
    # LINUX BUILD
    #      .--.
    #     |o_o |
    #     |:_/ |
    #    //   \ \
    #   (|     | )
    #  /'\_   _/`\
    #  \___)=(___/
    ################################################################
    LIB_PTHREAD='-lpthread'

    INC_LIBURING=$(pkg-config --cflags liburing)
    LIB_LIBURING=$(pkg-config --libs liburing)

    INC_LIBEVDEV=$(pkg-config --cflags libevdev)
    LIB_LIBEVDEV=$(pkg-config --libs libevdev)

    INC_WAYLAND_CLIENT=$(pkg-config --cflags wayland-client)
    LIB_WAYLAND_CLIENT=$(pkg-config --libs wayland-client)

    INC_XKBCOMMON=$(pkg-config --cflags xkbcommon)
    LIB_XKBCOMMON=$(pkg-config --libs xkbcommon)

    INC_LIBPIPEWIRE=$(pkg-config --cflags libpipewire-0.3)
    LIB_LIBPIPEWIRE=$(pkg-config --libs libpipewire-0.3)

    ### <PROJECT_NAME>
    # WaylandProtocolsInc= WaylandProtocolsSrc=
    . $ProjectRoot/protocol/build.sh

    src=""
    src="$src $WaylandProtocolsSrc"
    src="$src $ProjectRoot/src/main_linux.c"
    src="${src# }"

    # TODO: change output name
    output="$OutputDir/$OUTPUT_NAME"
    inc="-I$ProjectRoot/include $INC_LIBURING $INC_LIBEVDEV $INC_WAYLAND_CLIENT $INC_XKBCOMMON $INC_LIBPIPEWIRE $WaylandProtocolsInc"
    lib="$LIB_M $LIB_PTHREAD $LIB_LIBURING $LIB_LIBEVDEV $LIB_WAYLAND_CLIENT $LIB_XKBCOMMON $LIB_LIBPIPEWIRE"
    StartTimer
    "$cc" $cflags $ldflags $inc -o "$output" $src $lib
    [ $? -eq 0 ] && echo "$OUTPUT_NAME compiled in $(StopTimer) seconds."
  fi
fi

if [ $IsTestsEnabled -eq 1 ]; then
  . "$ProjectRoot/test/build.sh"
fi

Log "================================================================"
Log "Finished at $(date '+%Y-%m-%d %H:%M:%S')"
