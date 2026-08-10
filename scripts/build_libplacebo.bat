@echo off
setlocal enabledelayedexpansion

rem =============================================================
rem  Build libplacebo for Windows (MSVC/clang-cl via meson+ninja)
rem  Edit the variables below to match your machine.
rem =============================================================

rem --- Toolchain / SDK locations ---
set "VS_DIR=C:\Program Files\Microsoft Visual Studio\18\Community"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PYTHON_SCRIPTS=C:\Users\taoua\AppData\Local\Python\pythoncore-3.14-64\Scripts"
set "PKGCONF_DIR=C:\Program Files\pkgconf 3.0.5"

rem --- Source + output ---
set "LIBPLACEBO_SRC=C:\Users\taoua\libplacebo"
set "BUILD_TYPE=release"
set "PC_DIR=%TEMP%\opencode\pkgconfig"

rem --- Optional: deploy the result into the player project ---
set "PROJECT_EXTERNAL=C:\Dev\codotaku_video_player\external\libplacebo"

rem --- Optional libplacebo features (auto/enabled/disabled) ---
set "OPT_VULKAN=enabled"
set "OPT_SHADERC=enabled"
set "OPT_GLSLANG=auto"
set "OPT_D3D11=enabled"
set "OPT_LCMS=disabled"
set "OPT_DOVI=disabled"
set "OPT_LIBDOVI=disabled"
set "OPT_XXHASH=disabled"
set "OPT_UNWIND=disabled"

rem =============================================================
echo[==] Loading MSVC dev environment (vcvars64)...
call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat not found under "%VS_DIR%\VC\Auxiliary\Build"
    exit /b 1
)

set "PATH=%VS_DIR%\VC\Tools\Llvm\x64\bin;%PYTHON_SCRIPTS%;%PKGCONF_DIR%;%PATH%"
set CC=clang-cl
set CXX=clang-cl

rem --- Generate pkg-config files for the Vulkan SDK shader libs ---
rem (absolute paths so pkgconf's prefix relocation can't mangle them;
rem  forward slashes because pkgconf treats backslashes as escapes)
if not exist "%PC_DIR%" mkdir "%PC_DIR%"

set "VULKAN_SDK_FS=%VULKAN_SDK:\=/%"
echo includedir=%VULKAN_SDK_FS%/Include>"%PC_DIR%\shaderc.pc"
echo libdir=%VULKAN_SDK_FS%/Lib>>"%PC_DIR%\shaderc.pc"
echo.>>"%PC_DIR%\shaderc.pc"
echo Name: shaderc>>"%PC_DIR%\shaderc.pc"
echo Description: Vulkan shader compiler library>>"%PC_DIR%\shaderc.pc"
echo Version: 2024.1>>"%PC_DIR%\shaderc.pc"
echo Cflags: -I${includedir}>>"%PC_DIR%\shaderc.pc"
echo Libs: -L${libdir} -lshaderc_combined>>"%PC_DIR%\shaderc.pc"

echo includedir=%VULKAN_SDK_FS%/Include>"%PC_DIR%\spirv-cross-c-shared.pc"
echo libdir=%VULKAN_SDK_FS%/Lib>>"%PC_DIR%\spirv-cross-c-shared.pc"
echo.>>"%PC_DIR%\spirv-cross-c-shared.pc"
echo Name: spirv-cross-c-shared>>"%PC_DIR%\spirv-cross-c-shared.pc"
echo Description: SPIRV-Cross C API shared library>>"%PC_DIR%\spirv-cross-c-shared.pc"
echo Version: 0.66.0>>"%PC_DIR%\spirv-cross-c-shared.pc"
echo Cflags: -I${includedir}/spirv_cross>>"%PC_DIR%\spirv-cross-c-shared.pc"
echo Libs: -L${libdir} -lspirv-cross-c-shared>>"%PC_DIR%\spirv-cross-c-shared.pc"

set "PKG_CONFIG_PATH=%PC_DIR%"

rem --- Configure (always clean for a deterministic build) ---
cd /d "%LIBPLACEBO_SRC%"
if errorlevel 1 (
    echo [ERROR] libplacebo source not found at "%LIBPLACEBO_SRC%"
    exit /b 1
)

if exist build rmdir /s /q build

echo[==] Running meson setup...
meson setup build -Dbuildtype=%BUILD_TYPE% ^
    -Dvulkan=%OPT_VULKAN% ^
    -Dshaderc=%OPT_SHADERC% ^
    -Dglslang=%OPT_GLSLANG% ^
    -Dd3d11=%OPT_D3D11% ^
    -Dlcms=%OPT_LCMS% ^
    -Ddovi=%OPT_DOVI% ^
    -Dlibdovi=%OPT_LIBDOVI% ^
    -Dxxhash=%OPT_XXHASH% ^
    -Dunwind=%OPT_UNWIND% ^
    -Ddemos=false ^
    -Dvulkan-sdk=%VULKAN_SDK% ^
    -Dprefix=%LIBPLACEBO_SRC%\dist
if errorlevel 1 exit /b 1

rem --- Build ---
echo[==] Building with ninja...
ninja -C build
if errorlevel 1 exit /b 1

rem --- Install to dist ---
echo[==] Installing to %LIBPLACEBO_SRC%\dist...
ninja -C build install
if errorlevel 1 exit /b 1

rem --- Deploy into the player project ---
if defined PROJECT_EXTERNAL (
    echo[==] Deploying to %PROJECT_EXTERNAL%
    robocopy "%LIBPLACEBO_SRC%\dist" "%PROJECT_EXTERNAL%" /E /NFL /NDL /NJH /NJS /NP >nul
    if errorlevel 8 exit /b 1
)

echo.
echo ============================================================
echo  libplacebo build complete.
echo    DLL:     %LIBPLACEBO_SRC%\dist\bin\libplacebo-371.dll
echo    Import:  %LIBPLACEBO_SRC%\dist\lib\libplacebo.lib
echo    Headers: %LIBPLACEBO_SRC%\dist\include\libplacebo
echo ============================================================
endlocal
