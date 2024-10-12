# vi: set et ft=sh ts=2 sw=2 fenc=utf-8 :vi
################################################################
# TEST FUNCTIONS
################################################################

# void RunTest(testExecutable, failMessage)
RunTest() {
  executable="$1"
  failMessage="$2"

  "$executable"
  statusCode=$?
  if [ $statusCode -ne 0 ]; then
    echo "$failMessage code $statusCode"
    exit $statusCode
  fi
}

################################################################

### memory_test
inc="-I$ProjectRoot/include"
src="$ProjectRoot/test/memory_test.c"
output="$OutputDir/$(BasenameWithoutExtension "$src")"
lib="$LIB_M"
"$cc" $cflags $ldflags $inc -o "$output" $src $lib
RunTest "$output" "TEST memory failed."

### text_test
inc="-I$ProjectRoot/include"
src="$ProjectRoot/test/text_test.c"
output="$OutputDir/$(BasenameWithoutExtension "$src")"
lib="$LIB_M"
"$cc" $cflags $ldflags $inc -o "$output" $src $lib
RunTest "$output" "TEST text failed."
