set vcpkg_root=E:\vcpkg_mumble
cmake -G "Ninja" "-DBUILD_NUMBER=30" "-Dpackaging=ON" "-Dserver=OFF" "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md" "-Dstatic=ON" "-DCMAKE_TOOLCHAIN_FILE=%vcpkg_root%\scripts\buildsystems\vcpkg.cmake" "-DIce_HOME=%vcpkg_root%\installed\x64-windows-static-md" "-DCMAKE_BUILD_TYPE=Release" ..
pause