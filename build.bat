@echo off
setlocal

set MSVC=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717
set WINSDK=C:\Program Files (x86)\Windows Kits\10
set WINVER=10.0.26100.0

set "CC=%MSVC%\bin\Hostx64\x86\cl.exe"
set "LINKER=%MSVC%\bin\Hostx64\x86\link.exe"

set CFLAGS=/nologo /c /O2 /W3 /GS- /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS /D_USRDLL /DCOBJMACROS
set INCLUDES=/I"src" /I"%MSVC%\include" /I"%WINSDK%\Include\%WINVER%\ucrt" /I"%WINSDK%\Include\%WINVER%\um" /I"%WINSDK%\Include\%WINVER%\shared"

set SRCS=dllmain log d3d11_backend d3d11_state d3d11_texture glide_exports
set LIBS=kernel32.lib user32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib uuid.lib

if not exist build mkdir build

echo === Compiling ===
for %%f in (%SRCS%) do (
    "%CC%" %CFLAGS% %INCLUDES% /Fosrc\%%f.obj src\%%f.c
    if errorlevel 1 goto :fail
)

echo === Linking ===
set OBJS=
for %%f in (%SRCS%) do set OBJS=!OBJS! src\%%f.obj

"%LINKER%" /nologo /DLL /OUT:build\glide2x.dll /DEF:glide2x.def ^
    /LIBPATH:"%MSVC%\lib\x86" ^
    /LIBPATH:"%WINSDK%\Lib\%WINVER%\ucrt\x86" ^
    /LIBPATH:"%WINSDK%\Lib\%WINVER%\um\x86" ^
    src\dllmain.obj src\log.obj src\d3d11_backend.obj src\d3d11_state.obj src\d3d11_texture.obj src\glide_exports.obj ^
    %LIBS%
if errorlevel 1 goto :fail

echo === Build succeeded: build\glide2x.dll ===
goto :end

:fail
echo === BUILD FAILED ===
exit /b 1

:end
endlocal
