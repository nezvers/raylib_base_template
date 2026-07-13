rem Use this batch file to build box2d for Visual Studio
pushd "%~dp0"
rmdir /s /q build
mkdir build
pushd build
cmake ..
popd
popd
