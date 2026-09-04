@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if "%STREAMLINE_ROOT%"=="" set STREAMLINE_ROOT=%~dp0..\..\streamline
if not exist "%STREAMLINE_ROOT%\include\sl_dlss_g.h" (
    echo Set STREAMLINE_ROOT to a Streamline SDK directory containing include\sl_dlss_g.h
    exit /b 1
)
if not exist build mkdir build
pushd build
set COMMON=/nologo /W4 /EHsc /MT /O2 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN
cl %COMMON% /c ^
  ..\src\third_party\minhook\src\buffer.c ^
  ..\src\third_party\minhook\src\hook.c ^
  ..\src\third_party\minhook\src\trampoline.c ^
  ..\src\third_party\minhook\src\hde\hde64.c ^
  /I..\src\third_party\minhook\include /I..\src\third_party\minhook\src /I..\src\third_party\minhook\src\hde
if errorlevel 1 goto :fail
cl %COMMON% /std:c++20 /LD ^
  ..\src\loader.cpp ..\src\config.cpp ..\src\log.cpp ..\src\patches.cpp ^
  ..\src\streamline.cpp ..\src\ngx.cpp ..\src\ada_patch.cpp ..\src\provider_policy.cpp ^
  /I"%STREAMLINE_ROOT%\include" /I..\src\third_party\minhook\include ^
  /Fe:RTX40MFG.asi ^
  /link buffer.obj hook.obj trampoline.obj hde64.obj bcrypt.lib version.lib psapi.lib
if errorlevel 1 goto :fail
popd
echo Built build\RTX40MFG.asi
exit /b 0
:fail
popd
echo BUILD FAILED
exit /b 1
