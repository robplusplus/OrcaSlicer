set DEPS_PREFIX=%CD%\deps\build\OrcaSlicer_dep\usr\local
set BOOST_ROOT=%DEPS_PREFIX%
set Boost_DIR=%DEPS_PREFIX%\lib\cmake\Boost-1.84.0

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DORCA_TOOLS=ON -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="%DEPS_PREFIX%" ^
  -DCMAKE_C_FLAGS="/MP" -DCMAKE_CXX_FLAGS="/MP"

cmake --build build --config Release --target ALL_BUILD -- /m:%JOBS%
cmake --build build --config Release --target install   -- /m:%JOBS%