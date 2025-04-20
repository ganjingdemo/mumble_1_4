set start_time=%date% %time%

set vcpkg_root=F:\github\vcpkg_2025_02

rem cmake -G "Ninja" "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md" "-Dstatic=ON" "-DCMAKE_TOOLCHAIN_FILE=%vcpkg_root%/scripts/buildsystems/vcpkg.cmake" "-DIce_HOME=%vcpkg_root%/installed/x64-windows-static-md" "-DCMAKE_BUILD_TYPE=Release" -Ddebug-dependency-search=ON ..

rem pause "will use ninja to build"

set mid_time=%date% %time%

ninja

set end_time=%date% %time%

@echo start_time: %start_time%
@echo mid_time  : %mid_time%
@echo end_time  : %end_time%

pause