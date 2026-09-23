
if "Visual Studio 2017"=="%APPVEYOR_BUILD_WORKER_IMAGE%" goto :eof
if "Visual Studio 2015"=="%APPVEYOR_BUILD_WORKER_IMAGE%" goto :eof

mkdir build
cd build
cmake -G "Ninja" ..
ninja
cd..
