rem (Reconfigure deps, just in case)
cmake -S deps -B deps\build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

rem Build ONLY Boost first (target name is usually dep_boost). If that fails, run the umbrella 'deps' target.
cmake --build deps\build --config Release --target dep_boost -- /m:%NUMBER_OF_PROCESSORS%
rem If MSBuild says target not found, do:
cmake --build deps\build --config Release --target deps -- /m:%NUMBER_OF_PROCESSORS%

rem Now point CMake at the deps prefix and configure the main project
set DEPS_PREFIX=%CD%\deps\build\OrcaSlicer_dep\usr\local
set BOOST_ROOT=%DEPS_PREFIX%
set Boost_DIR=%DEPS_PREFIX%\lib\cmake\Boost-1.83.0

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DORCA_TOOLS=ON -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="%DEPS_PREFIX%" ^
  -DCMAKE_C_FLAGS="/MP" -DCMAKE_CXX_FLAGS="/MP"

cmake --build build --config Release --target ALL_BUILD -- /m:%NUMBER_OF_PROCESSORS%
